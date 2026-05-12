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
    error: float
    requested_step_v: float
    applied_step_v: float
    new_bias_v: float
    reason: str


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
    args: argparse.Namespace,
) -> tuple[BiasPoint, GradientRecord]:
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

    denom = obj_plus + obj_minus + 1e-24
    error = (obj_plus - obj_minus) / denom
    requested_step = -args.gain * error
    reason = "gradient"

    if obj_center <= obj_minus and obj_center <= obj_plus:
        requested_step = 0.0
        reason = "hold-center-best"
    elif abs(error) < args.deadband:
        requested_step = 0.0
        reason = "hold-deadband"

    applied_step = clamp(requested_step, -abs(args.max_step), abs(args.max_step))
    new_v = clamp(center_v + applied_step, min_v, max_v)
    if abs(new_v - (center_v + applied_step)) > 1e-9:
        reason += "+anchor-clamp"
    new_bias = set_axis_value(bias, axis, new_v)

    apply_bias(session, axis, new_v)
    session.progress(
        f"grad step {step_index:03d} axis={axis} center={center_v:+.4f}V "
        f"J-={metric_to_dbm(metric_minus):+.2f}dBm J0={metric_to_dbm(metric_center):+.2f}dBm "
        f"J+={metric_to_dbm(metric_plus):+.2f}dBm e={error:+.4f} "
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
        error=error,
        requested_step_v=requested_step,
        applied_step_v=applied_step,
        new_bias_v=new_v,
        reason=reason,
    )


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
                "error",
                "requested_step_v",
                "applied_step_v",
                "new_bias_v",
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
                f"{record.error:.9f}",
                f"{record.requested_step_v:.6f}",
                f"{record.applied_step_v:.6f}",
                f"{record.new_bias_v:.6f}",
                record.reason,
            ])


def plot_gradient(path: Path, records: list[GradientRecord], title: str) -> Path:
    import matplotlib.pyplot as plt

    path.parent.mkdir(parents=True, exist_ok=True)
    x = [record.step_index for record in records]

    fig, axes = plt.subplots(3, 1, figsize=(14, 10), sharex=True)
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
    parser.set_defaults(use_power=True)
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
                f"sequence={''.join(axes)} delta={args.delta:.4f}V gain={args.gain:.4f}V"
            )

            for step_index in range(1, args.cycles + 1):
                axis = axes[(step_index - 1) % len(axes)]
                bias, record = run_gradient_step(session, step_index, axis, bias, anchor, args)
                records.append(record)

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
