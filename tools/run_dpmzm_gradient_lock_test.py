# -*- coding: utf-8 -*-

from __future__ import annotations

import argparse
import csv
import math
import re
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

try:
    import serial
except ImportError as exc:
    raise SystemExit(
        "pyserial is required. Install it first, for example: pip install pyserial"
    ) from exc

from run_dpmzm_positive_branch_flow import (
    RAW_DATA_DIR,
    SerialSession,
    BiasPoint,
    MetricRow,
    clamp,
    metric_value,
    parse_applied_auto_result,
    parse_metric_fields,
)


PLOT_DIR = Path(r"C:\Users\Administrator\Desktop\DPMZM_contral_bais\simulation_image\lock_gradient")
BIAS_PATTERN = re.compile(
    r"bias target:\s+I=([+-]?\d+\.\d+)V\s+Q=([+-]?\d+\.\d+)V\s+P=([+-]?\d+\.\d+)V"
)


@dataclass
class AxisConfig:
    stage: str
    target: str
    blocks: int
    anchor_window_v: float


@dataclass
class GradientRecord:
    step_index: int
    axis: str
    bias_i: float
    bias_q: float
    bias_p: float
    v_minus: float
    v_center: float
    v_plus: float
    metric_minus: float
    metric_center: float
    metric_plus: float
    objective_minus: float
    objective_center: float
    objective_plus: float
    dbm_minus: float
    dbm_center: float
    dbm_plus: float
    pd_dc_center: float
    pd_dc_anchor: float
    pd_dc_rise_db: float
    pd_dc_rise_avg_db: float
    pd_dc_trigger_count: int
    error: float
    requested_step_v: float
    applied_step_v: float
    new_bias_v: float
    adaptive_scale: float
    effective_gain_v: float
    effective_max_step_v: float
    dc_guard_event: str
    reason: str


@dataclass
class AxisAdaptiveState:
    scale: float = 1.0
    last_direction: int = 0
    same_direction_count: int = 0


@dataclass
class DcGuardState:
    anchor_dc: float | None = None
    trigger_count: int = 0
    cooldown_steps: int = 0
    armed: bool = False
    rise_history_db: list[float] | None = None


def metric_to_dbm(vpeak: float) -> float:
    vrms = max(vpeak, 0.0) * 0.70710678
    mw = ((vrms * vrms) / 50.0) * 1000.0
    return 10.0 * math.log10(max(mw, 1e-15))


def objective_from_metric(metric: float, use_power: bool) -> float:
    metric = max(metric, 0.0)
    if use_power:
        return metric * metric
    return metric


def parse_bias_from_status(lines: list[str]) -> BiasPoint | None:
    for line in reversed(lines):
        match = BIAS_PATTERN.search(line)
        if match:
            return BiasPoint(
                i=float(match.group(1)),
                q=float(match.group(2)),
                p=float(match.group(3)),
            )
    return None


def axis_value(bias: BiasPoint, axis: str) -> float:
    if axis == "i":
        return bias.i
    if axis == "q":
        return bias.q
    if axis == "p":
        return bias.p
    raise ValueError(f"unknown axis {axis}")


def set_axis_value(bias: BiasPoint, axis: str, value: float) -> BiasPoint:
    if axis == "i":
        return BiasPoint(i=value, q=bias.q, p=bias.p)
    if axis == "q":
        return BiasPoint(i=bias.i, q=value, p=bias.p)
    if axis == "p":
        return BiasPoint(i=bias.i, q=bias.q, p=value)
    raise ValueError(f"unknown axis {axis}")


def axis_config(axis: str, iq_blocks: int, p_blocks: int, iq_anchor_window: float, p_anchor_window: float) -> AxisConfig:
    if axis == "i":
        return AxisConfig(stage="mitp", target="i", blocks=iq_blocks, anchor_window_v=iq_anchor_window)
    if axis == "q":
        return AxisConfig(stage="mitp", target="q", blocks=iq_blocks, anchor_window_v=iq_anchor_window)
    if axis == "p":
        return AxisConfig(stage="qtp", target="p", blocks=p_blocks, anchor_window_v=p_anchor_window)
    raise ValueError(f"unknown axis {axis}")


def axis_gain(args: argparse.Namespace, axis: str) -> float:
    value = {
        "i": args.i_gain,
        "q": args.q_gain,
        "p": args.p_gain,
    }.get(axis)
    return args.gain if value is None else value


def axis_max_step(args: argparse.Namespace, axis: str) -> float:
    value = {
        "i": args.i_max_step,
        "q": args.q_max_step,
        "p": args.p_max_step,
    }.get(axis)
    return args.max_step if value is None else value


def effective_axis_gain(args: argparse.Namespace, axis: str, state: AxisAdaptiveState) -> float:
    if not args.adaptive_step:
        return axis_gain(args, axis)
    return axis_gain(args, axis) * state.scale


def effective_axis_max_step(args: argparse.Namespace, axis: str, state: AxisAdaptiveState) -> float:
    base_max_step = axis_max_step(args, axis)
    if not args.adaptive_step:
        return base_max_step
    return min(base_max_step * state.scale, args.adaptive_hard_max_step)


def step_direction(step_v: float) -> int:
    if step_v > 1e-9:
        return 1
    if step_v < -1e-9:
        return -1
    return 0


def update_adaptive_state(
    state: AxisAdaptiveState,
    direction: int,
    reason: str,
    args: argparse.Namespace,
) -> None:
    if not args.adaptive_step:
        return

    if direction == 0 or reason.startswith("hold"):
        state.same_direction_count = 0
        if state.scale > 1.0:
            state.scale = max(1.0, state.scale * args.adaptive_shrink)
        return

    if state.last_direction != 0 and direction != state.last_direction:
        state.same_direction_count = 1
        state.scale = max(args.adaptive_min_scale, state.scale * args.adaptive_shrink)
    else:
        state.same_direction_count += 1
        if state.same_direction_count >= args.adaptive_same_direction:
            state.scale = min(args.adaptive_max_scale, state.scale * args.adaptive_growth)

    state.last_direction = direction


def dc_rise_db(dc_now: float, dc_anchor: float, dc_floor: float) -> float:
    ref = max(abs(dc_anchor), dc_floor)
    ratio = abs(dc_now) / ref
    return 20.0 * math.log10(max(ratio, 1e-12))


def update_dc_rise_history(state: DcGuardState, rise_db: float, window: int) -> float:
    if state.rise_history_db is None:
        state.rise_history_db = []
    state.rise_history_db.append(rise_db)
    window = max(1, window)
    if len(state.rise_history_db) > window:
        del state.rise_history_db[:-window]
    return sum(state.rise_history_db) / len(state.rise_history_db)


def evaluate_dc_guard(
    state: DcGuardState,
    dc_now: float,
    args: argparse.Namespace,
) -> tuple[float, float, float, int, bool, str]:
    if state.anchor_dc is None:
        state.anchor_dc = dc_now

    anchor_dc = state.anchor_dc
    rise_db = dc_rise_db(dc_now, anchor_dc, args.dc_guard_floor)
    avg_rise_db = update_dc_rise_history(state, rise_db, args.dc_guard_avg_window)
    event = "disabled"
    trigger = False

    if not args.dc_guard:
        return anchor_dc, rise_db, avg_rise_db, state.trigger_count, False, event

    if state.cooldown_steps > 0:
        state.cooldown_steps -= 1
        event = "cooldown"
        return anchor_dc, rise_db, avg_rise_db, state.trigger_count, False, event

    # Hysteresis: rising average arms the guard, but it is only released after
    # the averaged rise falls below a lower release threshold.
    if avg_rise_db <= args.dc_guard_release_db:
        state.armed = False
        state.trigger_count = 0
        event = "ok"
        return anchor_dc, rise_db, avg_rise_db, state.trigger_count, False, event

    if avg_rise_db <= args.dc_guard_threshold_db and not state.armed:
        state.trigger_count = 0
        event = "ok"
        return anchor_dc, rise_db, avg_rise_db, state.trigger_count, False, event

    if avg_rise_db > args.dc_guard_threshold_db:
        state.armed = True
    elif state.armed:
        event = "armed"
        return anchor_dc, rise_db, avg_rise_db, state.trigger_count, False, event

    state.trigger_count += 1
    event = "rise"
    if state.trigger_count >= args.dc_guard_trigger_count:
        trigger = True
        event = "trigger"
        state.trigger_count = 0
        state.cooldown_steps = args.dc_guard_cooldown_steps
    return anchor_dc, rise_db, avg_rise_db, state.trigger_count, trigger, event


def apply_bias(session: SerialSession, axis: str, value: float, wait_s: float = 0.15) -> None:
    session.send(f"dpmzm set bias {axis} {value:.4f}")
    session.read_for(wait_s)


def measure_single_point(
    session: SerialSession,
    config: AxisConfig,
    value: float,
    label: str,
    timeout_s: float,
) -> MetricRow:
    before_count = len(session.metric_rows)
    value = clamp(value)
    command = (
        f"dpmzm scan {config.stage} {config.target} "
        f"{value:.4f} {value:.4f} 0.010 {config.blocks}"
    )
    session.send(command)
    session.wait_scan_done(timeout_s=timeout_s, label=label, stage=config.stage, axis=config.target)

    rows: list[MetricRow] = []
    for fields in session.metric_rows[before_count:]:
        row = parse_metric_fields(fields)
        if row is None:
            continue
        if row.stage == config.stage and row.target == config.target:
            rows.append(row)
    if not rows:
        raise RuntimeError(f"{label}: no metric row captured")
    return rows[-1]


def run_auto_first(session: SerialSession, coarse_timeout: float, fine_timeout: float) -> BiasPoint:
    session.progress("running firmware auto coarse/fine before gradient test")
    session.send("dpmzm auto coarse")
    coarse_line = session.wait_for_any(
        ["[dpmzm][auto] applied coarse result", "[dpmzm][auto] coarse failed", "[dpmzm][auto] failed"],
        coarse_timeout,
        "auto coarse",
    )
    if coarse_line is None or "failed" in coarse_line:
        raise RuntimeError("auto coarse failed or timed out")

    session.send("dpmzm auto fine")
    fine_line = session.wait_for_any(
        ["[dpmzm][auto] applied fine result", "[dpmzm][auto] fine failed"],
        fine_timeout,
        "auto fine",
    )
    if fine_line is None or "failed" in fine_line:
        raise RuntimeError("auto fine failed or timed out")

    point = parse_applied_auto_result(session.lines, "fine")
    if point is None:
        raise RuntimeError("could not parse applied fine result")
    return point


def run_gradient_step(
    session: SerialSession,
    step_index: int,
    axis: str,
    bias: BiasPoint,
    anchor: BiasPoint,
    adaptive_state: AxisAdaptiveState,
    dc_guard_state: DcGuardState,
    args: argparse.Namespace,
) -> tuple[BiasPoint, GradientRecord, bool]:
    config = axis_config(axis, args.iq_blocks, args.p_blocks, args.iq_anchor_window, args.p_anchor_window)
    center_v = axis_value(bias, axis)
    anchor_v = axis_value(anchor, axis)
    min_v = max(args.min_bias, anchor_v - config.anchor_window_v)
    max_v = min(args.max_bias, anchor_v + config.anchor_window_v)
    center_v = clamp(center_v, min_v, max_v)

    v_minus = clamp(center_v - args.delta, min_v, max_v)
    v_plus = clamp(center_v + args.delta, min_v, max_v)
    label_prefix = f"grad {step_index:03d} {axis}"

    row_minus = measure_single_point(session, config, v_minus, f"{label_prefix} minus", args.point_timeout)
    row_center = measure_single_point(session, config, center_v, f"{label_prefix} center", args.point_timeout)
    row_plus = measure_single_point(session, config, v_plus, f"{label_prefix} plus", args.point_timeout)

    metric_minus = metric_value(row_minus)
    metric_center = metric_value(row_center)
    metric_plus = metric_value(row_plus)
    obj_minus = objective_from_metric(metric_minus, args.use_power)
    obj_center = objective_from_metric(metric_center, args.use_power)
    obj_plus = objective_from_metric(metric_plus, args.use_power)
    pd_dc_anchor, pd_dc_rise, pd_dc_rise_avg, pd_dc_count, dc_guard_trigger, dc_guard_event = evaluate_dc_guard(
        dc_guard_state,
        row_center.dc,
        args,
    )

    denom = obj_plus + obj_minus + 1e-24
    error = (obj_plus - obj_minus) / denom
    scale_used = adaptive_state.scale
    gain = effective_axis_gain(args, axis, adaptive_state)
    max_step = effective_axis_max_step(args, axis, adaptive_state)
    requested_step = -gain * error
    reason = "gradient"

    if obj_center <= obj_minus and obj_center <= obj_plus:
        requested_step = 0.0
        reason = "hold-center-best"
    elif abs(error) < args.deadband:
        requested_step = 0.0
        reason = "hold-deadband"

    limited_step = clamp(requested_step, -abs(max_step), abs(max_step))
    new_v = clamp(center_v + limited_step, min_v, max_v)
    applied_step = new_v - center_v
    if abs(applied_step - limited_step) > 1e-9:
        reason += "+anchor-clamp"
    new_bias = set_axis_value(bias, axis, new_v)
    update_adaptive_state(adaptive_state, step_direction(applied_step), reason, args)

    apply_bias(session, axis, new_v)
    session.progress(
        f"grad step {step_index:03d} axis={axis} center={center_v:+.4f}V "
        f"J-={metric_to_dbm(metric_minus):+.2f}dBm J0={metric_to_dbm(metric_center):+.2f}dBm "
        f"J+={metric_to_dbm(metric_plus):+.2f}dBm e={error:+.4f} "
        f"dc={row_center.dc:+.6f}V rise={pd_dc_rise:+.2f}dB avg={pd_dc_rise_avg:+.2f}dB "
        f"guard={dc_guard_event}/{pd_dc_count} "
        f"gain={gain:.4f}V max={max_step:.4f}V scale={scale_used:.2f}->{adaptive_state.scale:.2f} "
        f"step={applied_step:+.5f}V -> {new_v:+.4f}V {reason}"
    )

    return new_bias, GradientRecord(
        step_index=step_index,
        axis=axis,
        bias_i=new_bias.i,
        bias_q=new_bias.q,
        bias_p=new_bias.p,
        v_minus=v_minus,
        v_center=center_v,
        v_plus=v_plus,
        metric_minus=metric_minus,
        metric_center=metric_center,
        metric_plus=metric_plus,
        objective_minus=obj_minus,
        objective_center=obj_center,
        objective_plus=obj_plus,
        dbm_minus=metric_to_dbm(metric_minus),
        dbm_center=metric_to_dbm(metric_center),
        dbm_plus=metric_to_dbm(metric_plus),
        pd_dc_center=row_center.dc,
        pd_dc_anchor=pd_dc_anchor,
        pd_dc_rise_db=pd_dc_rise,
        pd_dc_rise_avg_db=pd_dc_rise_avg,
        pd_dc_trigger_count=pd_dc_count,
        error=error,
        requested_step_v=requested_step,
        applied_step_v=applied_step,
        new_bias_v=new_v,
        adaptive_scale=scale_used,
        effective_gain_v=gain,
        effective_max_step_v=max_step,
        dc_guard_event=dc_guard_event,
        reason=reason,
    ), dc_guard_trigger


def local_sweep_values(center: float, window: float, step: float, min_v: float, max_v: float) -> list[float]:
    start = clamp(center - abs(window), min_v, max_v)
    stop = clamp(center + abs(window), min_v, max_v)
    step = abs(step)
    if step <= 0.0:
        raise ValueError("local sweep step must be positive")
    values: list[float] = []
    value = start
    while value <= stop + step * 0.5:
        values.append(clamp(round(value, 6), min_v, max_v))
        value += step
    if not values or abs(values[-1] - stop) > step * 0.25:
        values.append(stop)
    return sorted(set(values))


def choose_recheck_row(rows: list[MetricRow], dc_limit_abs: float, args: argparse.Namespace) -> MetricRow:
    valid_rows = [row for row in rows if abs(row.dc) <= dc_limit_abs]
    candidates = valid_rows if valid_rows else rows
    return min(candidates, key=lambda row: objective_from_metric(metric_value(row), args.use_power))


def run_dc_guard_recheck(
    session: SerialSession,
    bias: BiasPoint,
    anchor: BiasPoint,
    dc_guard_state: DcGuardState,
    args: argparse.Namespace,
) -> BiasPoint:
    if not args.dc_guard_recheck_sequence:
        return bias

    ref_abs = max(abs(dc_guard_state.anchor_dc or 0.0), args.dc_guard_floor)
    dc_limit_abs = ref_abs * (10.0 ** (args.dc_guard_threshold_db / 20.0))
    session.progress(
        "dc guard recheck start: "
        f"anchor_dc={dc_guard_state.anchor_dc:+.6f}V limit_abs={dc_limit_abs:.6f}V "
        f"sequence={args.dc_guard_recheck_sequence}"
    )

    best_dc_for_anchor: float | None = None
    for axis in [char.lower() for char in args.dc_guard_recheck_sequence if char.lower() in {"i", "q", "p"}]:
        config = axis_config(axis, args.iq_blocks, args.p_blocks, args.iq_anchor_window, args.p_anchor_window)
        center_v = axis_value(bias, axis)
        anchor_v = axis_value(anchor, axis)
        min_v = max(args.min_bias, anchor_v - config.anchor_window_v)
        max_v = min(args.max_bias, anchor_v + config.anchor_window_v)
        window = args.dc_recheck_p_window if axis == "p" else args.dc_recheck_iq_window
        values = local_sweep_values(center_v, window, args.dc_recheck_step, min_v, max_v)

        rows: list[MetricRow] = []
        for index, value in enumerate(values, start=1):
            row = measure_single_point(
                session,
                config,
                value,
                f"dcguard {axis} {index:02d}/{len(values):02d}",
                args.point_timeout,
            )
            rows.append(row)

        selected = choose_recheck_row(rows, dc_limit_abs, args)
        selected_metric = metric_value(selected)
        selected_dbm = metric_to_dbm(selected_metric)
        valid_count = sum(1 for row in rows if abs(row.dc) <= dc_limit_abs)
        session.progress(
            f"dc guard recheck axis={axis}: points={len(rows)} valid_dc={valid_count} "
            f"select {selected.sweep:+.4f}V metric={selected_dbm:+.2f}dBm dc={selected.dc:+.6f}V"
        )
        apply_bias(session, axis, selected.sweep, wait_s=0.25)
        bias = set_axis_value(bias, axis, selected.sweep)

        if best_dc_for_anchor is None or abs(selected.dc) < abs(best_dc_for_anchor):
            best_dc_for_anchor = selected.dc

    if args.dc_guard_update_anchor and best_dc_for_anchor is not None:
        old_anchor = dc_guard_state.anchor_dc
        old_ref = max(abs(old_anchor or 0.0), args.dc_guard_floor)
        if old_anchor is None or abs(best_dc_for_anchor) <= old_ref:
            dc_guard_state.anchor_dc = best_dc_for_anchor
            session.progress(
                f"dc guard anchor updated: {old_anchor!s} -> {dc_guard_state.anchor_dc:+.6f}V"
            )
        else:
            session.progress(
                f"dc guard anchor kept: {old_anchor:+.6f}V, recheck best dc={best_dc_for_anchor:+.6f}V"
            )

    return bias


def write_gradient_csv(path: Path, records: list[GradientRecord]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as file_obj:
        writer = csv.writer(file_obj)
        writer.writerow(
            [
                "step_index",
                "axis",
                "bias_i",
                "bias_q",
                "bias_p",
                "v_minus",
                "v_center",
                "v_plus",
                "metric_minus",
                "metric_center",
                "metric_plus",
                "objective_minus",
                "objective_center",
                "objective_plus",
                "dbm_minus",
                "dbm_center",
                "dbm_plus",
                "pd_dc_center",
                "pd_dc_anchor",
                "pd_dc_rise_db",
                "pd_dc_rise_avg_db",
                "pd_dc_trigger_count",
                "error",
                "requested_step_v",
                "applied_step_v",
                "new_bias_v",
                "adaptive_scale",
                "effective_gain_v",
                "effective_max_step_v",
                "dc_guard_event",
                "reason",
            ]
        )
        for record in records:
            writer.writerow([
                record.step_index,
                record.axis,
                f"{record.bias_i:.6f}",
                f"{record.bias_q:.6f}",
                f"{record.bias_p:.6f}",
                f"{record.v_minus:.6f}",
                f"{record.v_center:.6f}",
                f"{record.v_plus:.6f}",
                f"{record.metric_minus:.9f}",
                f"{record.metric_center:.9f}",
                f"{record.metric_plus:.9f}",
                f"{record.objective_minus:.12e}",
                f"{record.objective_center:.12e}",
                f"{record.objective_plus:.12e}",
                f"{record.dbm_minus:.3f}",
                f"{record.dbm_center:.3f}",
                f"{record.dbm_plus:.3f}",
                f"{record.pd_dc_center:.9f}",
                f"{record.pd_dc_anchor:.9f}",
                f"{record.pd_dc_rise_db:.3f}",
                f"{record.pd_dc_rise_avg_db:.3f}",
                record.pd_dc_trigger_count,
                f"{record.error:.9f}",
                f"{record.requested_step_v:.6f}",
                f"{record.applied_step_v:.6f}",
                f"{record.new_bias_v:.6f}",
                f"{record.adaptive_scale:.6f}",
                f"{record.effective_gain_v:.6f}",
                f"{record.effective_max_step_v:.6f}",
                record.dc_guard_event,
                record.reason,
            ])


def plot_gradient(path: Path, records: list[GradientRecord], title: str) -> Path:
    import matplotlib.pyplot as plt

    path.parent.mkdir(parents=True, exist_ok=True)
    x = [record.step_index for record in records]

    fig, axes = plt.subplots(4, 1, figsize=(14, 12), sharex=True)
    fig.suptitle(title)

    axes[0].plot(x, [record.bias_i for record in records], marker="o", label="I bias")
    axes[0].plot(x, [record.bias_q for record in records], marker="o", label="Q bias")
    axes[0].plot(x, [record.bias_p for record in records], marker="o", label="P bias")
    axes[0].set_ylabel("Bias (V)")
    axes[0].grid(True, alpha=0.3)
    axes[0].legend()

    for axis_name, marker in [("p", "o"), ("i", "s"), ("q", "^")]:
        xs = [record.step_index for record in records if record.axis == axis_name]
        ys = [record.dbm_center for record in records if record.axis == axis_name]
        axes[1].plot(xs, ys, marker=marker, label=f"{axis_name.upper()} center metric")
    axes[1].set_ylabel("Center metric (dBm)")
    axes[1].grid(True, alpha=0.3)
    axes[1].legend()

    axes[2].plot(x, [record.error for record in records], marker="o", label="normalized gradient")
    axes[2].plot(x, [record.applied_step_v * 1000.0 for record in records], marker="s", label="applied step (mV)")
    axes[2].axhline(0.0, color="0.4", linewidth=0.8)
    axes[2].set_xlabel("Gradient step")
    axes[2].set_ylabel("Error / step")
    axes[2].grid(True, alpha=0.3)
    axes[2].legend()

    axes[3].plot(x, [record.pd_dc_center for record in records], marker="o", label="PD DC center (V)")
    axes[3].set_xlabel("Gradient step")
    axes[3].set_ylabel("PD DC (V)")
    axes[3].grid(True, alpha=0.3)
    axes3b = axes[3].twinx()
    axes3b.plot(x, [record.pd_dc_rise_db for record in records], color="tab:red", marker="s", alpha=0.35, label="PD DC rise raw (dB)")
    axes3b.plot(x, [record.pd_dc_rise_avg_db for record in records], color="tab:red", linewidth=2.0, label="PD DC rise avg (dB)")
    axes3b.set_ylabel("Rise vs anchor (dB)")
    lines, labels = axes[3].get_legend_handles_labels()
    lines2, labels2 = axes3b.get_legend_handles_labels()
    axes[3].legend(lines + lines2, labels + labels2, loc="best")

    fig.tight_layout()
    fig.savefig(path, dpi=150)
    plt.close(fig)
    return path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="PC-side DPMZM coordinate gradient-descent lock test.")
    parser.add_argument("--port", default="COM8")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--output-dir", type=Path, default=RAW_DATA_DIR)
    parser.add_argument("--plot-dir", type=Path, default=PLOT_DIR)
    parser.add_argument("--startup-wait", type=float, default=0.5)
    parser.add_argument("--cycles", type=int, default=30, help="Number of single-axis gradient updates.")
    parser.add_argument("--sequence", default="piq", help="Axis sequence, default piq.")
    parser.add_argument("--delta", type=float, default=0.01, help="Probe half-step in volts.")
    parser.add_argument("--gain", type=float, default=0.003, help="Gradient gain in volts.")
    parser.add_argument("--max-step", type=float, default=0.003, help="Maximum applied update in volts.")
    parser.add_argument("--i-gain", type=float, default=None, help="Override I-axis gradient gain in volts.")
    parser.add_argument("--q-gain", type=float, default=None, help="Override Q-axis gradient gain in volts.")
    parser.add_argument("--p-gain", type=float, default=None, help="Override P-axis gradient gain in volts.")
    parser.add_argument("--i-max-step", type=float, default=None, help="Override I-axis maximum update in volts.")
    parser.add_argument("--q-max-step", type=float, default=None, help="Override Q-axis maximum update in volts.")
    parser.add_argument("--p-max-step", type=float, default=None, help="Override P-axis maximum update in volts.")
    adaptive_group = parser.add_mutually_exclusive_group()
    adaptive_group.add_argument("--adaptive-step", dest="adaptive_step", action="store_true")
    adaptive_group.add_argument("--no-adaptive-step", dest="adaptive_step", action="store_false")
    parser.add_argument("--adaptive-hard-max-step", type=float, default=0.010, help="Hard cap for adaptive per-axis update in volts.")
    parser.add_argument("--adaptive-min-scale", type=float, default=0.5, help="Minimum adaptive gain/max-step scale.")
    parser.add_argument("--adaptive-max-scale", type=float, default=4.0, help="Maximum adaptive gain/max-step scale before hard cap.")
    parser.add_argument("--adaptive-growth", type=float, default=1.5, help="Scale multiplier after repeated same-direction moves.")
    parser.add_argument("--adaptive-shrink", type=float, default=0.5, help="Scale multiplier after hold or direction reversal.")
    parser.add_argument("--adaptive-same-direction", type=int, default=3, help="Repeated same-direction moves before adaptive growth.")
    dc_guard_group = parser.add_mutually_exclusive_group()
    dc_guard_group.add_argument("--dc-guard", dest="dc_guard", action="store_true")
    dc_guard_group.add_argument("--no-dc-guard", dest="dc_guard", action="store_false")
    parser.add_argument("--dc-guard-threshold-db", type=float, default=4.0, help="Arm/trigger when averaged PD DC rise exceeds this many dB.")
    parser.add_argument("--dc-guard-release-db", type=float, default=2.0, help="Release DC guard only after averaged PD DC rise falls below this many dB.")
    parser.add_argument("--dc-guard-avg-window", type=int, default=5, help="Sliding average window for PD DC rise in gradient samples.")
    parser.add_argument("--dc-guard-trigger-count", type=int, default=3, help="Consecutive averaged PD DC rise detections required before recheck.")
    parser.add_argument("--dc-guard-floor", type=float, default=1e-5, help="Minimum absolute PD DC reference in volts.")
    parser.add_argument("--dc-guard-cooldown-steps", type=int, default=12, help="Gradient steps to wait after one DC-guard recheck.")
    parser.add_argument("--dc-guard-recheck-sequence", default="iqp", help="Axes to locally recheck after a PD DC guard trigger.")
    parser.add_argument("--dc-recheck-iq-window", type=float, default=0.08, help="I/Q local recheck half-window in volts.")
    parser.add_argument("--dc-recheck-p-window", type=float, default=0.10, help="P local recheck half-window in volts.")
    parser.add_argument("--dc-recheck-step", type=float, default=0.01, help="Local recheck step in volts.")
    parser.add_argument("--dc-guard-update-anchor", action="store_true", help="Allow PD DC anchor to update after a successful recheck.")
    parser.add_argument("--deadband", type=float, default=0.03, help="Normalized gradient deadband.")
    parser.add_argument("--iq-blocks", type=int, default=4)
    parser.add_argument("--p-blocks", type=int, default=10)
    parser.add_argument("--iq-anchor-window", type=float, default=0.20)
    parser.add_argument("--p-anchor-window", type=float, default=0.30)
    parser.add_argument("--min-bias", type=float, default=-9.0)
    parser.add_argument("--max-bias", type=float, default=9.0)
    parser.add_argument("--point-timeout", type=float, default=60.0)
    parser.add_argument("--run-auto-first", action="store_true")
    parser.add_argument(
        "--skip-setup",
        action="store_true",
        help="Do not send lock stop / dump metrics / pilot-open setup commands before the gradient test.",
    )
    parser.add_argument("--coarse-timeout", type=float, default=360.0)
    parser.add_argument("--fine-timeout", type=float, default=360.0)
    parser.add_argument("--initial-i", type=float, default=None)
    parser.add_argument("--initial-q", type=float, default=None)
    parser.add_argument("--initial-p", type=float, default=None)
    parser.add_argument("--use-magnitude", dest="use_power", action="store_false")
    parser.add_argument("--no-plot", action="store_true")
    parser.set_defaults(use_power=True, adaptive_step=False, dc_guard=False)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    axes = [char.lower() for char in args.sequence if char.lower() in {"p", "i", "q"}]
    if not axes:
        raise SystemExit("sequence must contain at least one of p/i/q")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    args.plot_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y-%m-%d_%H%M%S")
    log_path = args.output_dir / f"{stamp}_dpmzm_gradient_lock_serial.log"
    progress_path = args.output_dir / f"{stamp}_dpmzm_gradient_lock_progress.log"
    csv_path = args.output_dir / f"{stamp}_dpmzm_gradient_lock_steps.csv"
    image_path = args.plot_dir / f"{stamp}_dpmzm_gradient_lock_steps.png"

    records: list[GradientRecord] = []
    adaptive_states = {axis: AxisAdaptiveState() for axis in ("p", "i", "q")}
    dc_guard_state = DcGuardState()
    exit_code = 0

    try:
        with serial.Serial(args.port, baudrate=args.baud, timeout=0.05, write_timeout=2) as ser:
            session = SerialSession(ser, log_path, progress_path)
            session.progress(f"log: {log_path}")
            session.read_for(args.startup_wait)

            if not args.skip_setup:
                for command, wait_s in [
                    ("dpmzm lock stop", 0.8),
                    ("dpmzm set dump metrics", 0.8),
                    ("dpmzm set pilot-open on", 1.0),
                ]:
                    session.send(command)
                    session.read_for(wait_s)

            if args.initial_i is not None:
                apply_bias(session, "i", args.initial_i, wait_s=0.4)
            if args.initial_q is not None:
                apply_bias(session, "q", args.initial_q, wait_s=0.4)
            if args.initial_p is not None:
                apply_bias(session, "p", args.initial_p, wait_s=0.4)

            if args.run_auto_first:
                bias = run_auto_first(session, args.coarse_timeout, args.fine_timeout)
            else:
                session.send("dpmzm status")
                session.read_for(1.5)
                bias = parse_bias_from_status(session.lines)
                if bias is None:
                    if args.initial_i is None or args.initial_q is None or args.initial_p is None:
                        raise RuntimeError("could not parse current bias from status; provide --initial-i/q/p")
                    bias = BiasPoint(i=args.initial_i, q=args.initial_q, p=args.initial_p)

            anchor = BiasPoint(i=bias.i, q=bias.q, p=bias.p)
            session.progress(
                f"gradient anchor: I={anchor.i:+.4f}V Q={anchor.q:+.4f}V P={anchor.p:+.4f}V; "
                f"sequence={''.join(axes)} delta={args.delta:.4f}V "
                f"gain I/Q/P={axis_gain(args, 'i'):.4f}/{axis_gain(args, 'q'):.4f}/{axis_gain(args, 'p'):.4f}V "
                f"max_step I/Q/P={axis_max_step(args, 'i'):.4f}/{axis_max_step(args, 'q'):.4f}/{axis_max_step(args, 'p'):.4f}V "
                f"adaptive={'on' if args.adaptive_step else 'off'} hard_max={args.adaptive_hard_max_step:.4f}V "
                f"dc_guard={'on' if args.dc_guard else 'off'} threshold={args.dc_guard_threshold_db:.1f}dB"
            )

            for step_index in range(1, args.cycles + 1):
                axis = axes[(step_index - 1) % len(axes)]
                bias, record, dc_guard_trigger = run_gradient_step(
                    session,
                    step_index,
                    axis,
                    bias,
                    anchor,
                    adaptive_states[axis],
                    dc_guard_state,
                    args,
                )
                records.append(record)
                if dc_guard_trigger:
                    record.dc_guard_event = "trigger-recheck"
                    record.reason += "+dc-guard-recheck"
                    bias = run_dc_guard_recheck(session, bias, anchor, dc_guard_state, args)

            write_gradient_csv(csv_path, records)
            session.progress(f"gradient csv saved: {csv_path}")
            session.progress(f"serial log saved: {log_path}")
            session.progress(f"progress log saved: {progress_path}")

    except serial.SerialException as exc:
        print(f"Serial error on {args.port}: {exc}", file=sys.stderr)
        return 10
    except Exception as exc:
        print(f"gradient test failed: {exc}", file=sys.stderr)
        exit_code = 2

    if records and not args.no_plot:
        try:
            plot_gradient(
                image_path,
                records,
                f"DPMZM PC-side gradient lock test ({stamp})",
            )
            print(f"PLOT={image_path}")
        except Exception as exc:
            print(f"plot failed: {exc}", file=sys.stderr)
            exit_code = 3

    print(f"SERIAL_LOG={log_path}")
    print(f"PROGRESS_LOG={progress_path}")
    print(f"GRADIENT_CSV={csv_path}")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
