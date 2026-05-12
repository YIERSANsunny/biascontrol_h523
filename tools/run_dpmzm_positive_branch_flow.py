# -*- coding: utf-8 -*-

from __future__ import annotations

import argparse
import csv
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


RAW_DATA_DIR = Path(r"C:\Users\Administrator\Desktop\DPMZM_contral_bais\raw data")
SIM_IMAGE_DIR = Path(r"C:\Users\Administrator\Desktop\DPMZM_contral_bais\simulation_image\auto_flow")
METRIC_PREFIX = "DPMZMCSV,"
MATP_PLATFORM_THRESHOLD = 0.65
MATP_PLATFORM_MIN_POINTS = 3
MATP_PLATFORM_MIN_WIDTH_V = 1.0
MATP_PLATFORM_WIDTH_REF_V = 2.0
MATP_PLATFORM_WIDTH_WEIGHT = 0.2

METRIC_HEADER = [
    "stage",
    "target_channel",
    "sweep_value",
    "sweep_unit",
    "bias_I",
    "bias_Q",
    "bias_P",
    "pilot_mode",
    "blocks",
    "mag_fI",
    "mag_fQ",
    "mag_fdiff_200",
    "mag_fsum_2200",
    "dc",
]


@dataclass
class BiasPoint:
    i: float = 0.0
    q: float = 0.0
    p: float = 0.0


@dataclass
class AutoFinePicks:
    positive_p: float | None = None
    i: float | None = None
    q: float | None = None
    applied_final: BiasPoint | None = None

    def complete(self) -> bool:
        return self.positive_p is not None and self.i is not None and self.q is not None

    def positive_branch(self) -> BiasPoint:
        if not self.complete():
            raise RuntimeError("auto fine positive-branch picks are incomplete")
        return BiasPoint(i=float(self.i), q=float(self.q), p=float(self.positive_p))


@dataclass
class MetricRow:
    stage: str
    target: str
    sweep: float
    blocks: int
    mag_fi: float
    mag_fq: float
    mag_fdiff: float
    mag_fsum: float
    dc: float


@dataclass
class ProbeMetric:
    metric: float
    dbm: float
    center_best: bool = False
    direction: str = ""


@dataclass
class MatpPlatform:
    left_v: float
    right_v: float
    center_v: float
    width_v: float
    point_count: int
    mean_pnorm: float
    width_norm: float
    ripple: float
    score: float
    touches_edge: bool = False
    fallback: bool = False


@dataclass
class AnchorGuardAxisState:
    direction_streak: int = 0
    degrade_streak: int = 0


class SerialSession:
    def __init__(self, ser: serial.Serial, log_path: Path, progress_path: Path):
        self.ser = ser
        self.log_path = log_path
        self.progress_path = progress_path
        self.buffer = ""
        self.lines: list[str] = []
        self.metric_rows: list[list[str]] = []

    def log(self, text: str) -> None:
        with self.log_path.open("a", encoding="utf-8") as file_obj:
            file_obj.write(text)
        sys.stdout.write(text)
        sys.stdout.flush()

    def progress(self, text: str) -> None:
        stamp = datetime.now().strftime("%H:%M:%S")
        line = f"[{stamp}] {text}\n"
        with self.progress_path.open("a", encoding="utf-8") as file_obj:
            file_obj.write(line)
        print(line, end="", flush=True)

    def _record_line(self, line: str) -> None:
        clean = line.strip()
        if not clean:
            return
        self.lines.append(clean)
        if clean.startswith(METRIC_PREFIX):
            fields = [field.strip() for field in clean[len(METRIC_PREFIX):].split(",")]
            if len(fields) == len(METRIC_HEADER):
                self.metric_rows.append(fields)

    def read_for(self, seconds: float) -> list[str]:
        deadline = time.time() + seconds
        start_len = len(self.lines)
        while time.time() < deadline:
            payload = self.ser.read(4096)
            if payload:
                text = payload.decode("utf-8", errors="replace")
                self.log(text)
                self.buffer += text
                while "\n" in self.buffer:
                    line, self.buffer = self.buffer.split("\n", 1)
                    self._record_line(line)
            else:
                time.sleep(0.02)
        return self.lines[start_len:]

    def send(self, command: str) -> None:
        self.progress(f">>> {command}")
        self.log(f"\n>>> {command}\n")
        self.ser.write((command + "\r\n").encode("ascii", errors="ignore"))
        self.ser.flush()

    def wait_for_any(self, needles: list[str], timeout_s: float, label: str) -> str | None:
        start = time.time()
        seen = len(self.lines)
        last_progress = start
        while time.time() - start < timeout_s:
            self.read_for(0.25)
            for line in self.lines[seen:]:
                for needle in needles:
                    if needle in line:
                        self.progress(f"{label} matched: {needle}")
                        return line
            seen = len(self.lines)
            now = time.time()
            if now - last_progress >= 20.0:
                self.progress(f"{label} still running, elapsed {now - start:.0f}s")
                last_progress = now
        self.progress(f"{label} timeout after {timeout_s:.0f}s")
        return None

    def wait_scan_done(self, timeout_s: float, label: str, stage: str, axis: str) -> float:
        start = time.time()
        seen = len(self.lines)
        last_progress = start
        scan_active = False
        start_pattern = re.compile(
            rf"\[dpmzm\]\s+scan start:\s+stage={re.escape(stage)}\s+target={re.escape(axis)}\b"
        )
        best_pattern = re.compile(r"\[dpmzm\]\s+scan done: best .* at ([+-]?\d+\.\d+) V")

        while time.time() - start < timeout_s:
            self.read_for(0.25)
            for line in self.lines[seen:]:
                if start_pattern.search(line):
                    scan_active = True
                    continue
                if not scan_active:
                    continue
                if "[dpmzm] scan failed" in line:
                    raise RuntimeError(f"{label} failed")
                match = best_pattern.search(line)
                if match:
                    best = float(match.group(1))
                    self.progress(f"{label} best={best:+.3f}V")
                    return best
            seen = len(self.lines)
            now = time.time()
            if now - last_progress >= 20.0:
                self.progress(f"{label} still running, elapsed {now - start:.0f}s")
                last_progress = now
        raise TimeoutError(f"{label} timeout")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run the validated DPMZM positive-branch flow: auto coarse, auto fine, "
            "restore first positive P branch, then small-window P/I/Q/P scans."
        )
    )
    parser.add_argument("--port", default="COM9")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--output-dir", type=Path, default=RAW_DATA_DIR)
    parser.add_argument("--plot-dir", type=Path, default=SIM_IMAGE_DIR)
    parser.add_argument(
        "--no-plot",
        action="store_true",
        help="Skip plotting the all-stages PNG after the scan finishes.",
    )
    parser.add_argument("--startup-wait", type=float, default=0.8)
    parser.add_argument("--coarse-timeout", type=float, default=420.0)
    parser.add_argument("--fine-timeout", type=float, default=780.0)
    parser.add_argument(
        "--small-window",
        type=float,
        default=None,
        help="Override both P and I/Q final small-window half-widths.",
    )
    parser.add_argument("--p-small-window", type=float, default=0.30)
    parser.add_argument("--iq-small-window", type=float, default=0.10)
    parser.add_argument("--small-step", type=float, default=0.01)
    parser.add_argument("--prelock-iq-recheck-window", type=float, default=0.08)
    parser.add_argument("--prelock-iq-recheck-step", type=float, default=0.01)
    parser.add_argument(
        "--disable-prelock-iq-recheck",
        action="store_true",
        help="Skip the final +/- window I/Q MITP recheck before starting closed-loop control.",
    )
    parser.add_argument("--iq-blocks", type=int, default=4)
    parser.add_argument("--p-blocks", type=int, default=6)
    parser.add_argument("--p-turn-step", type=float, default=0.10)
    parser.add_argument("--p-turn-window", type=float, default=0.10)
    parser.add_argument("--p-turn-max-shifts", type=int, default=8)
    parser.add_argument("--iq-turn-step", type=float, default=0.10)
    parser.add_argument("--iq-turn-window", type=float, default=0.10)
    parser.add_argument(
        "--iq-turn-max-shifts",
        type=int,
        default=-1,
        help="Maximum I/Q turning-search shifts; negative means search until bracketed or blocked.",
    )
    parser.add_argument(
        "--disable-p-turn-search",
        action="store_true",
        help="Use the old fixed P small-window scan instead of adaptive P turning-point search.",
    )
    parser.add_argument(
        "--disable-iq-turn-search",
        action="store_true",
        help="Use the old fixed I/Q small-window scans instead of adaptive I/Q turning-point search.",
    )
    parser.add_argument("--initial-i", type=float, default=0.0)
    parser.add_argument("--initial-q", type=float, default=0.0)
    parser.add_argument("--initial-p", type=float, default=0.0)
    parser.add_argument(
        "--manual-p-qtp-seed",
        action="store_true",
        help=(
            "Skip auto coarse/fine. Use initial I/Q, run one P-QTP sweep to "
            "seed P, then continue with the normal turning-point flow."
        ),
    )
    parser.add_argument(
        "--iq-matp-seed",
        action="store_true",
        help=(
            "Before manual P-QTP seeding, run I/Q MATP coarse scans and use "
            "high first-order-power plateau centers as the initial I/Q biases."
        ),
    )
    parser.add_argument("--manual-matp-start", type=float, default=-9.0)
    parser.add_argument("--manual-matp-stop", type=float, default=9.0)
    parser.add_argument("--manual-matp-step", type=float, default=0.5)
    parser.add_argument("--manual-p-start", type=float, default=-9.0)
    parser.add_argument("--manual-p-stop", type=float, default=9.0)
    parser.add_argument("--manual-p-step", type=float, default=0.5)
    parser.add_argument("--manual-mitp-start", type=float, default=-9.0)
    parser.add_argument("--manual-mitp-stop", type=float, default=9.0)
    parser.add_argument("--manual-mitp-step", type=float, default=0.5)
    parser.add_argument(
        "--skip-initial-bias",
        action="store_true",
        help="Start auto coarse from the board's current I/Q/P biases instead of the default 0/0/0 V.",
    )
    parser.add_argument(
        "--skip-auto-fine",
        action="store_true",
        help="Use the auto-coarse result directly, then refine with adaptive turning-point scans.",
    )
    parser.add_argument(
        "--pilot-off-at-end",
        action="store_true",
        help="Turn continuous onboard pilot off after finishing. Default keeps it on for observation.",
    )
    parser.add_argument(
        "--no-lock-at-end",
        action="store_true",
        help="Do not start closed-loop bias control after the final scan. Default starts it.",
    )
    parser.add_argument(
        "--anchor-guard-duration",
        type=float,
        default=0.0,
        help=(
            "After lock start, run script-side I/Q anchor guard for this many seconds. "
            "Default disables post-lock guard monitoring."
        ),
    )
    parser.add_argument("--anchor-guard-interval", type=float, default=30.0)
    parser.add_argument("--anchor-guard-first-delay", type=float, default=5.0)
    parser.add_argument(
        "--anchor-guard-degrade-db",
        type=float,
        default=8.0,
        help=(
            "Count one severe-degrade event when center metric worsens by this many dB. "
            "A recheck is triggered only after consecutive events."
        ),
    )
    parser.add_argument("--anchor-guard-direction-count", type=int, default=2)
    parser.add_argument("--anchor-guard-degrade-count", type=int, default=2)
    parser.add_argument("--anchor-guard-iq-window", type=float, default=0.08)
    parser.add_argument("--anchor-guard-iq-step", type=float, default=0.01)
    parser.add_argument("--anchor-guard-p-window", type=float, default=0.12)
    parser.add_argument("--anchor-guard-p-step", type=float, default=0.01)
    return parser.parse_args()


def clamp(value: float, lo: float = -9.0, hi: float = 9.0) -> float:
    return max(lo, min(hi, value))


def parse_metric_fields(fields: list[str]) -> MetricRow | None:
    if len(fields) != len(METRIC_HEADER):
        return None
    return MetricRow(
        stage=fields[0],
        target=fields[1],
        sweep=float(fields[2]),
        blocks=int(float(fields[8])),
        mag_fi=float(fields[9]),
        mag_fq=float(fields[10]),
        mag_fdiff=float(fields[11]),
        mag_fsum=float(fields[12]),
        dc=float(fields[13]),
    )


def metric_value(row: MetricRow) -> float:
    if row.stage == "qtp":
        return row.mag_fsum
    if row.target == "q":
        return row.mag_fq
    return row.mag_fi


def choose_mitp_valley_by_dc(rows: list[MetricRow], label: str, session: SerialSession) -> float:
    if not rows:
        raise RuntimeError(f"{label}: no metric rows captured")

    valleys: list[MetricRow] = []
    for index in range(1, len(rows) - 1):
        left = metric_value(rows[index - 1])
        center = metric_value(rows[index])
        right = metric_value(rows[index + 1])
        if center <= left and center <= right:
            valleys.append(rows[index])

    if not valleys:
        fallback = min(rows, key=metric_value)
        session.progress(
            f"{label}: no local valley; fallback to global metric minimum "
            f"{fallback.sweep:+.3f}V metric={metric_value(fallback):.9f} dc={fallback.dc:.6f}"
        )
        return fallback.sweep

    selected = min(valleys, key=lambda row: row.dc)
    summary = ", ".join(
        f"{row.sweep:+.3f}V(m={metric_value(row):.9f},dc={row.dc:.6f})"
        for row in valleys[:6]
    )
    if len(valleys) > 6:
        summary += ", ..."
    session.progress(
        f"{label}: local valleys={len(valleys)} [{summary}] -> "
        f"select {selected.sweep:+.3f}V by lowest DC"
    )
    return selected.sweep


def choose_matp_plateau_center(rows: list[MetricRow], label: str, session: SerialSession) -> float:
    if not rows:
        raise RuntimeError(f"{label}: no metric rows captured")

    rows = sorted(rows, key=lambda row: row.sweep)
    powers = [metric_value(row) ** 2 for row in rows]
    p_min = min(powers)
    p_max = max(powers)
    span = p_max - p_min
    if span <= max(p_max, 1.0) * 1e-12:
        fallback = max(rows, key=metric_value)
        session.progress(
            f"{label}: flat MATP power; fallback to global first-order maximum "
            f"{fallback.sweep:+.3f}V metric={metric_value(fallback):.9f}"
        )
        return fallback.sweep

    pnorm = [(power - p_min) / span for power in powers]
    flags = [value >= MATP_PLATFORM_THRESHOLD for value in pnorm]

    runs: list[tuple[int, int]] = []
    start: int | None = None
    for index, flag in enumerate(flags):
        if flag and start is None:
            start = index
        elif not flag and start is not None:
            runs.append((start, index - 1))
            start = None
    if start is not None:
        runs.append((start, len(flags) - 1))

    platforms: list[MatpPlatform] = []
    for left_index, right_index in runs:
        point_count = right_index - left_index + 1
        left_v = rows[left_index].sweep
        right_v = rows[right_index].sweep
        width_v = abs(right_v - left_v)
        if point_count < MATP_PLATFORM_MIN_POINTS or width_v < MATP_PLATFORM_MIN_WIDTH_V:
            continue

        values = pnorm[left_index:right_index + 1]
        weights = [max(value, 1e-9) for value in values]
        x_values = [row.sweep for row in rows[left_index:right_index + 1]]
        center_v = sum(x * weight for x, weight in zip(x_values, weights)) / sum(weights)
        mean_pnorm = sum(values) / len(values)
        width_norm = min(width_v / MATP_PLATFORM_WIDTH_REF_V, 1.0)
        ripple = (sum((value - mean_pnorm) ** 2 for value in values) / len(values)) ** 0.5
        score = mean_pnorm + MATP_PLATFORM_WIDTH_WEIGHT * width_norm
        platforms.append(
            MatpPlatform(
                left_v=left_v,
                right_v=right_v,
                center_v=center_v,
                width_v=width_v,
                point_count=point_count,
                mean_pnorm=mean_pnorm,
                width_norm=width_norm,
                ripple=ripple,
                score=score,
                touches_edge=left_index == 0 or right_index == len(rows) - 1,
            )
        )

    if not platforms:
        best_index = max(range(len(rows)), key=lambda item: pnorm[item])
        fallback = rows[best_index]
        session.progress(
            f"{label}: no high-power platform "
            f"(threshold={MATP_PLATFORM_THRESHOLD:.2f}, min_width={MATP_PLATFORM_MIN_WIDTH_V:.2f}V); "
            f"fallback to global first-order maximum {fallback.sweep:+.3f}V "
            f"metric={metric_value(fallback):.9f} pnorm={pnorm[best_index]:.3f}"
        )
        return fallback.sweep

    platforms.sort(key=lambda item: item.score, reverse=True)
    selected = platforms[0]
    summary = "; ".join(
        f"{index + 1}:{item.left_v:+.3f}..{item.right_v:+.3f}V "
        f"center={item.center_v:+.3f}V score={item.score:.3f} "
        f"mean={item.mean_pnorm:.3f} width={item.width_v:.3f}V"
        for index, item in enumerate(platforms[:4])
    )
    session.progress(
        f"{label}: high-power platforms={len(platforms)} "
        f"(threshold={MATP_PLATFORM_THRESHOLD:.2f}) [{summary}] -> "
        f"select center {selected.center_v:+.3f}V"
    )
    return selected.center_v


def parse_auto_fine_picks(lines: list[str]) -> AutoFinePicks:
    picks = AutoFinePicks()
    state = ""
    state_pattern = re.compile(r"\[dpmzm\]\[auto\]\s+state=([A-Z0-9_]+)")
    pick_pattern = re.compile(
        r"\[dpmzm\]\[auto\]\s+fine pick:\s+stage=(\w+)\s+target=(\w+).*best=([+-]?\d+\.\d+)V"
    )
    final_pattern = re.compile(
        r"\[dpmzm\]\[auto\]\s+applied fine result:\s+"
        r"I=([+-]?\d+\.\d+)V\s+Q=([+-]?\d+\.\d+)V\s+P=([+-]?\d+\.\d+)V"
    )

    for line in lines:
        state_match = state_pattern.search(line)
        if state_match:
            state = state_match.group(1)
            continue

        pick_match = pick_pattern.search(line)
        if pick_match:
            stage = pick_match.group(1).lower()
            target = pick_match.group(2).lower()
            best = float(pick_match.group(3))
            if state in ("FINE_SCAN_P_WIDE", "FINE_SCAN_P_FINE") and stage == "qtp" and target == "p":
                picks.positive_p = best
            elif state in ("FINE_SCAN_I_WIDE", "FINE_SCAN_I_FINE") and stage == "mitp" and target == "i":
                picks.i = best
            elif state in ("FINE_SCAN_Q_WIDE", "FINE_SCAN_Q_FINE") and stage == "mitp" and target == "q":
                picks.q = best
            continue

        final_match = final_pattern.search(line)
        if final_match:
            picks.applied_final = BiasPoint(
                i=float(final_match.group(1)),
                q=float(final_match.group(2)),
                p=float(final_match.group(3)),
            )

    return picks


def parse_applied_auto_result(lines: list[str], label: str) -> BiasPoint | None:
    pattern = re.compile(
        rf"\[dpmzm\]\[auto\]\s+applied {re.escape(label)} result:\s+"
        r"I=([+-]?\d+\.\d+)V\s+Q=([+-]?\d+\.\d+)V\s+P=([+-]?\d+\.\d+)V"
    )
    for line in lines:
        match = pattern.search(line)
        if match:
            return BiasPoint(
                i=float(match.group(1)),
                q=float(match.group(2)),
                p=float(match.group(3)),
            )
    return None


def scan_and_apply(
    session: SerialSession,
    stage: str,
    axis: str,
    center_v: float,
    window_v: float,
    step_v: float,
    blocks: int,
) -> float:
    start_v = clamp(center_v - window_v)
    stop_v = clamp(center_v + window_v)
    command = f"dpmzm scan {stage} {axis} {start_v:.3f} {stop_v:.3f} {step_v:.3f} {blocks}"
    label = f"small {stage}-{axis}"
    session.send(command)
    best = session.wait_scan_done(timeout_s=180.0, label=label, stage=stage, axis=axis)
    session.send(f"dpmzm set bias {axis} {best:.3f}")
    session.read_for(1.0)
    return best


def scan_collect(
    session: SerialSession,
    stage: str,
    axis: str,
    start_v: float,
    stop_v: float,
    step_v: float,
    blocks: int,
    label: str,
    timeout_s: float = 180.0,
) -> tuple[float, list[MetricRow]]:
    before_metric_count = len(session.metric_rows)
    command = f"dpmzm scan {stage} {axis} {start_v:.3f} {stop_v:.3f} {step_v:.3f} {blocks}"
    session.send(command)
    best = session.wait_scan_done(timeout_s=timeout_s, label=label, stage=stage, axis=axis)

    rows: list[MetricRow] = []
    for fields in session.metric_rows[before_metric_count:]:
        row = parse_metric_fields(fields)
        if row is None:
            continue
        if row.stage == stage and row.target == axis:
            rows.append(row)
    rows.sort(key=lambda row: row.sweep)
    return best, rows


def scan_and_apply_collect(
    session: SerialSession,
    stage: str,
    axis: str,
    center_v: float,
    window_v: float,
    step_v: float,
    blocks: int,
    label: str,
) -> float:
    start_v = clamp(center_v - window_v)
    stop_v = clamp(center_v + window_v)
    best, _ = scan_collect(session, stage, axis, start_v, stop_v, step_v, blocks, label)
    session.send(f"dpmzm set bias {axis} {best:.3f}")
    session.read_for(1.0)
    return best


def probe_axis_center(session: SerialSession, axis: str, label: str) -> ProbeMetric:
    start = time.time()
    seen = len(session.lines)
    saw_probe = False
    metric_pattern = re.compile(r"metric0:\s+([0-9.]+)\s+\(([+-]?\d+\.\d+) dBm\).*best=(yes|no)")
    direction_pattern = re.compile(r"e:\s+[+-]?\d+\.\d+\s+direction=([A-Za-z0-9_-]+)")
    metric: float | None = None
    dbm: float | None = None
    center_best = False

    session.send(f"dpmzm lock probe {axis}")
    while time.time() - start < 30.0:
        session.read_for(0.25)
        for line in session.lines[seen:]:
            if f"[dpmzm][lock] probe axis={axis}" in line:
                saw_probe = True
                continue
            if not saw_probe:
                continue
            match = metric_pattern.search(line)
            if match:
                metric = float(match.group(1))
                dbm = float(match.group(2))
                center_best = match.group(3) == "yes"
                continue
            direction_match = direction_pattern.search(line)
            if direction_match and metric is not None and dbm is not None:
                direction = direction_match.group(1)
                session.progress(
                    f"{label}: {axis.upper()} metric0={metric:.9f} "
                    f"({dbm:+.2f} dBm) best={'yes' if center_best else 'no'} "
                    f"direction={direction}"
                )
                return ProbeMetric(
                    metric=metric,
                    dbm=dbm,
                    center_best=center_best,
                    direction=direction,
                )
        seen = len(session.lines)
    raise TimeoutError(f"{label}: timeout waiting for lock probe {axis}")


def refresh_iq_anchor_baseline(session: SerialSession, label: str) -> dict[str, ProbeMetric]:
    return {
        "i": probe_axis_center(session, "i", label),
        "q": probe_axis_center(session, "q", label),
    }


def anchor_direction_valid(probe: ProbeMetric) -> bool:
    return probe.direction in ("hold", "hold-center", "hold-weak")


def update_anchor_baseline(old: ProbeMetric, current: ProbeMetric, alpha: float = 0.25) -> ProbeMetric:
    return ProbeMetric(
        metric=current.metric,
        dbm=old.dbm + alpha * (current.dbm - old.dbm),
        center_best=current.center_best,
        direction=current.direction,
    )


def stop_lock_and_wait(session: SerialSession, label: str) -> bool:
    session.send("dpmzm lock stop")
    line = session.wait_for_any(["[dpmzm][lock] stop"], 30.0, label)
    session.read_for(0.5)
    return line is not None


def recheck_anchor_axes(
    session: SerialSession,
    branch: BiasPoint,
    args: argparse.Namespace,
    axes: list[str],
    label_prefix: str,
) -> dict[str, ProbeMetric]:
    if "i" in axes:
        branch.i = scan_and_apply_collect(
            session,
            "mitp",
            "i",
            branch.i,
            args.anchor_guard_iq_window,
            args.anchor_guard_iq_step,
            args.iq_blocks,
            f"{label_prefix} I recheck",
        )
    if "q" in axes:
        branch.q = scan_and_apply_collect(
            session,
            "mitp",
            "q",
            branch.q,
            args.anchor_guard_iq_window,
            args.anchor_guard_iq_step,
            args.iq_blocks,
            f"{label_prefix} Q recheck",
        )

    branch.p = scan_and_apply_collect(
        session,
        "qtp",
        "p",
        branch.p,
        args.anchor_guard_p_window,
        args.anchor_guard_p_step,
        args.p_blocks,
        f"{label_prefix} P recheck",
    )
    return refresh_iq_anchor_baseline(session, f"{label_prefix} baseline refresh")


def run_anchor_guard(
    session: SerialSession,
    branch: BiasPoint,
    args: argparse.Namespace,
    baseline: dict[str, ProbeMetric],
) -> None:
    if args.anchor_guard_duration <= 0.0:
        return

    start = time.time()
    next_check = start + max(0.0, args.anchor_guard_first_delay)
    guard_state = {
        "i": AnchorGuardAxisState(),
        "q": AnchorGuardAxisState(),
    }
    session.progress(
        "anchor guard start: "
        f"duration={args.anchor_guard_duration:.0f}s "
        f"interval={args.anchor_guard_interval:.0f}s "
        f"threshold={args.anchor_guard_degrade_db:.1f}dB "
        f"direction_count={args.anchor_guard_direction_count} "
        f"degrade_count={args.anchor_guard_degrade_count}"
    )

    while time.time() - start < args.anchor_guard_duration:
        wait_s = min(0.5, max(0.0, next_check - time.time()))
        if wait_s > 0:
            session.read_for(wait_s)
            continue

        session.progress("anchor guard check: pause P lock and probe I/Q")
        if not stop_lock_and_wait(session, "anchor guard stop lock"):
            session.progress("anchor guard stop not confirmed; skip probe to avoid command overwrite")
            session.send("dpmzm lock start")
            session.read_for(0.8)
            next_check += args.anchor_guard_interval
            continue

        try:
            current = refresh_iq_anchor_baseline(session, "anchor guard probe")
        except TimeoutError as exc:
            session.progress(f"anchor guard probe skipped: {exc}")
            session.send("dpmzm lock start")
            session.read_for(1.0)
            next_check += args.anchor_guard_interval
            continue
        triggered_axes: list[str] = []
        for axis in ("i", "q"):
            degrade_db = current[axis].dbm - baseline[axis].dbm
            off_valley = not anchor_direction_valid(current[axis])
            severe_degrade = degrade_db >= args.anchor_guard_degrade_db
            state = guard_state[axis]
            state.direction_streak = state.direction_streak + 1 if off_valley else 0
            state.degrade_streak = state.degrade_streak + 1 if severe_degrade else 0
            session.progress(
                f"anchor {axis.upper()}: current={current[axis].dbm:+.2f} dBm "
                f"baseline={baseline[axis].dbm:+.2f} dBm degrade={degrade_db:+.2f} dB "
                f"direction={current[axis].direction} "
                f"dir_streak={state.direction_streak} deg_streak={state.degrade_streak}"
            )
            if state.direction_streak >= args.anchor_guard_direction_count:
                triggered_axes.append(axis)
            elif state.degrade_streak >= args.anchor_guard_degrade_count:
                triggered_axes.append(axis)

        if triggered_axes:
            session.progress("anchor guard triggered: " + ",".join(axis.upper() for axis in triggered_axes))
            try:
                baseline = recheck_anchor_axes(session, branch, args, triggered_axes, "anchor")
                for confirm_round in range(2):
                    off_axes = [
                        axis for axis in ("i", "q")
                        if not anchor_direction_valid(baseline[axis])
                    ]
                    if not off_axes:
                        break
                    session.progress(
                        "anchor guard post-recheck confirm triggered: " +
                        ",".join(axis.upper() for axis in off_axes)
                    )
                    baseline = recheck_anchor_axes(
                        session,
                        branch,
                        args,
                        off_axes,
                        f"anchor confirm {confirm_round + 1}",
                    )
                for axis in ("i", "q"):
                    guard_state[axis] = AnchorGuardAxisState()
            except TimeoutError as exc:
                session.progress(f"anchor guard baseline refresh failed: {exc}")
        else:
            for axis in ("i", "q"):
                if anchor_direction_valid(current[axis]):
                    baseline[axis] = update_anchor_baseline(baseline[axis], current[axis])
            session.progress("anchor guard: I/Q anchors still valid, no recheck")

        session.send("dpmzm lock start")
        session.read_for(1.0)
        next_check += args.anchor_guard_interval


def turning_point_scan_and_apply(
    session: SerialSession,
    stage: str,
    axis: str,
    center_v: float,
    fallback_window_v: float,
    final_window_v: float,
    final_step_v: float,
    turn_step_v: float,
    max_shifts: int,
    blocks: int,
    label_prefix: str,
) -> float:
    center = clamp(center_v)
    bracketed_center: float | None = None

    attempt = 0
    visited_centers: set[float] = set()
    while max_shifts < 0 or attempt <= max_shifts:
        probe_center = center
        if center - turn_step_v < -9.0:
            probe_center = clamp(-9.0 + turn_step_v)
        elif center + turn_step_v > 9.0:
            probe_center = clamp(9.0 - turn_step_v)

        center_key = round(probe_center, 6)
        if center_key in visited_centers:
            session.progress(
                f"{label_prefix} repeated center {probe_center:+.3f}V; "
                "stop unbounded search to avoid oscillation"
            )
            break
        visited_centers.add(center_key)

        start_v = clamp(probe_center - turn_step_v)
        stop_v = clamp(probe_center + turn_step_v)
        label = f"{label_prefix} probe {attempt + 1}"
        _, rows = scan_collect(session, stage, axis, start_v, stop_v, turn_step_v, blocks, label)
        if len(rows) < 3:
            break

        left = rows[0]
        mid = min(rows, key=lambda row: abs(row.sweep - probe_center))
        right = rows[-1]
        left_m = metric_value(left)
        mid_m = metric_value(mid)
        right_m = metric_value(right)

        if mid_m <= left_m and mid_m <= right_m:
            bracketed_center = mid.sweep
            session.progress(f"{label_prefix} bracketed around {bracketed_center:+.3f}V")
            break

        best_row = min((left, mid, right), key=metric_value)
        next_center = clamp(best_row.sweep)
        direction = "left" if next_center < center else "right"
        session.progress(
            f"{label_prefix} moves {direction}: center {center:+.3f}V -> {next_center:+.3f}V"
        )
        if abs(next_center - center) < 1e-6:
            break
        center = next_center
        attempt += 1

    if bracketed_center is None:
        session.progress(f"{label_prefix} fallback: fixed small-window scan")
        return scan_and_apply_collect(
            session,
            stage,
            axis,
            center,
            fallback_window_v,
            final_step_v,
            blocks,
            f"{label_prefix} fallback",
        )

    best, rows = scan_collect(
        session,
        stage,
        axis,
        clamp(bracketed_center - final_window_v),
        clamp(bracketed_center + final_window_v),
        final_step_v,
        blocks,
        f"{label_prefix} fine",
    )

    if rows:
        left_edge = min(row.sweep for row in rows)
        right_edge = max(row.sweep for row in rows)
        edge_margin = max(final_step_v * 1.5, 0.015)
        if abs(best - left_edge) <= edge_margin or abs(best - right_edge) <= edge_margin:
            session.progress(f"{label_prefix} fine best near edge, rescan around {best:+.3f}V")
            best, _ = scan_collect(
                session,
                stage,
                axis,
                clamp(best - final_window_v),
                clamp(best + final_window_v),
                final_step_v,
                blocks,
                f"{label_prefix} fine edge",
            )

    session.send(f"dpmzm set bias {axis} {best:.3f}")
    session.read_for(1.0)
    return best


def save_metrics(path: Path, rows: list[list[str]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as file_obj:
        writer = csv.writer(file_obj)
        writer.writerow(METRIC_HEADER)
        writer.writerows(rows)


def main() -> int:
    args = parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y-%m-%d_%H%M%S")
    stem = f"{stamp}_dpmzm_positive_branch_flow"
    log_path = args.output_dir / f"{stem}_serial.log"
    progress_path = args.output_dir / f"{stem}_progress.log"
    metrics_path = args.output_dir / f"{stem}_metrics.csv"

    with serial.Serial(args.port, args.baud, timeout=0.05, dsrdtr=False, rtscts=False) as ser:
        try:
            ser.dtr = False
            ser.rts = False
        except Exception:
            pass

        time.sleep(args.startup_wait)
        ser.reset_input_buffer()
        session = SerialSession(ser, log_path, progress_path)
        session.progress(f"log: {log_path}")

        for command in (
            "dpmzm lock stop",
            "dpmzm set dump metrics",
            "dpmzm set pilot-open on",
            "dpmzm status",
        ):
            session.send(command)
            session.read_for(1.0)

        if not args.skip_initial_bias:
            session.progress(
                f"setting initial bias: I={args.initial_i:+.3f}V "
                f"Q={args.initial_q:+.3f}V P={args.initial_p:+.3f}V"
            )
            for axis, value in (
                ("i", args.initial_i),
                ("q", args.initial_q),
                ("p", args.initial_p),
            ):
                session.send(f"dpmzm set bias {axis} {value:.3f}")
                session.read_for(1.0)
            session.send("dpmzm status")
            session.read_for(1.0)

        p_small_window = args.small_window if args.small_window is not None else args.p_small_window
        iq_small_window = args.small_window if args.small_window is not None else args.iq_small_window

        if args.manual_p_qtp_seed:
            i_seed = args.initial_i
            q_seed = args.initial_q
            if args.iq_matp_seed:
                session.progress(
                    "I/Q MATP seed: "
                    f"sweep={args.manual_matp_start:+.3f}..{args.manual_matp_stop:+.3f}V "
                    f"step={args.manual_matp_step:.3f}V blocks={args.iq_blocks}"
                )
                _, i_matp_rows = scan_collect(
                    session,
                    "matp",
                    "i",
                    clamp(args.manual_matp_start),
                    clamp(args.manual_matp_stop),
                    args.manual_matp_step,
                    args.iq_blocks,
                    "manual seed I-MATP",
                    timeout_s=args.coarse_timeout,
                )
                i_seed = choose_matp_plateau_center(i_matp_rows, "manual seed I-MATP", session)
                session.send(f"dpmzm set bias i {i_seed:.3f}")
                session.read_for(1.0)

                _, q_matp_rows = scan_collect(
                    session,
                    "matp",
                    "q",
                    clamp(args.manual_matp_start),
                    clamp(args.manual_matp_stop),
                    args.manual_matp_step,
                    args.iq_blocks,
                    "manual seed Q-MATP",
                    timeout_s=args.coarse_timeout,
                )
                q_seed = choose_matp_plateau_center(q_matp_rows, "manual seed Q-MATP", session)
                session.send(f"dpmzm set bias q {q_seed:.3f}")
                session.read_for(1.0)

            session.progress(
                "manual P-QTP seed: "
                f"I={i_seed:+.3f}V Q={q_seed:+.3f}V "
                f"P sweep={args.manual_p_start:+.3f}..{args.manual_p_stop:+.3f}V "
                f"step={args.manual_p_step:.3f}V blocks={args.p_blocks}"
            )
            p_seed, _ = scan_collect(
                session,
                "qtp",
                "p",
                clamp(args.manual_p_start),
                clamp(args.manual_p_stop),
                args.manual_p_step,
                args.p_blocks,
                "manual seed P-QTP",
                timeout_s=args.coarse_timeout,
            )
            session.send(f"dpmzm set bias p {p_seed:.3f}")
            session.read_for(1.0)

            _, i_mitp_rows = scan_collect(
                session,
                "mitp",
                "i",
                clamp(args.manual_mitp_start),
                clamp(args.manual_mitp_stop),
                args.manual_mitp_step,
                args.iq_blocks,
                "manual seed I-MITP",
                timeout_s=args.coarse_timeout,
            )
            i_seed = choose_mitp_valley_by_dc(i_mitp_rows, "manual seed I-MITP", session)
            session.send(f"dpmzm set bias i {i_seed:.3f}")
            session.read_for(1.0)

            _, q_mitp_rows = scan_collect(
                session,
                "mitp",
                "q",
                clamp(args.manual_mitp_start),
                clamp(args.manual_mitp_stop),
                args.manual_mitp_step,
                args.iq_blocks,
                "manual seed Q-MITP",
                timeout_s=args.coarse_timeout,
            )
            q_seed = choose_mitp_valley_by_dc(q_mitp_rows, "manual seed Q-MITP", session)
            session.send(f"dpmzm set bias q {q_seed:.3f}")
            session.read_for(1.0)

            session.progress(
                "manual P-QTP rescan after I/Q MITP: "
                f"I={i_seed:+.3f}V Q={q_seed:+.3f}V "
                f"P sweep={args.manual_p_start:+.3f}..{args.manual_p_stop:+.3f}V "
                f"step={args.manual_p_step:.3f}V blocks={args.p_blocks}"
            )
            p_rescan_seed, _ = scan_collect(
                session,
                "qtp",
                "p",
                clamp(args.manual_p_start),
                clamp(args.manual_p_stop),
                args.manual_p_step,
                args.p_blocks,
                "manual seed P-QTP after MITP",
                timeout_s=args.coarse_timeout,
            )
            branch = BiasPoint(i=i_seed, q=q_seed, p=p_rescan_seed)
            session.progress(
                f"manual turn-search seed after MITP: "
                f"I={branch.i:+.3f}V Q={branch.q:+.3f}V P={branch.p:+.3f}V"
            )
        else:
            before_coarse_line_count = len(session.lines)
            session.send("dpmzm auto coarse")
            coarse_line = session.wait_for_any(
                ["[dpmzm][auto] applied coarse result", "[dpmzm][auto] coarse failed", "[dpmzm][auto] failed"],
                args.coarse_timeout,
                "auto coarse",
            )
            if coarse_line is None or "failed" in coarse_line:
                return 2

            if args.skip_auto_fine:
                branch = parse_applied_auto_result(session.lines[before_coarse_line_count:], "coarse")
                if branch is None:
                    session.progress("failed to parse I/Q/P seed from auto coarse")
                    return 4
                session.progress(
                    f"turn-search seed from auto coarse: I={branch.i:+.3f}V Q={branch.q:+.3f}V P={branch.p:+.3f}V"
                )
            else:
                before_fine_line_count = len(session.lines)
                session.send("dpmzm auto fine")
                fine_line = session.wait_for_any(
                    ["[dpmzm][auto] applied fine result", "[dpmzm][auto] fine failed"],
                    args.fine_timeout,
                    "auto fine",
                )
                if fine_line is None or "failed" in fine_line:
                    return 3

                picks = parse_auto_fine_picks(session.lines[before_fine_line_count:])
                if not picks.complete():
                    session.progress("failed to parse positive-branch P/I/Q picks from auto fine")
                    return 4

                branch = picks.positive_branch()
                session.progress(
                    f"positive branch from auto fine: I={branch.i:+.3f}V Q={branch.q:+.3f}V P={branch.p:+.3f}V"
                )
                if picks.applied_final is not None:
                    session.progress(
                        f"auto fine final ignored: I={picks.applied_final.i:+.3f}V "
                        f"Q={picks.applied_final.q:+.3f}V P={picks.applied_final.p:+.3f}V"
                    )

        for axis, value in (("i", branch.i), ("q", branch.q), ("p", branch.p)):
            session.send(f"dpmzm set bias {axis} {value:.3f}")
            session.read_for(1.0)
        session.send("dpmzm status")
        session.read_for(1.0)

        if args.disable_p_turn_search:
            branch.p = scan_and_apply(session, "qtp", "p", branch.p, p_small_window, args.small_step, args.p_blocks)
        else:
            branch.p = turning_point_scan_and_apply(
                session,
                "qtp",
                "p",
                branch.p,
                p_small_window,
                args.p_turn_window,
                args.small_step,
                args.p_turn_step,
                args.p_turn_max_shifts,
                args.p_blocks,
                "p-turn",
            )
        if args.disable_iq_turn_search:
            branch.i = scan_and_apply(session, "mitp", "i", branch.i, iq_small_window, args.small_step, args.iq_blocks)
            branch.q = scan_and_apply(session, "mitp", "q", branch.q, iq_small_window, args.small_step, args.iq_blocks)
        else:
            branch.i = turning_point_scan_and_apply(
                session,
                "mitp",
                "i",
                branch.i,
                iq_small_window,
                args.iq_turn_window,
                args.small_step,
                args.iq_turn_step,
                args.iq_turn_max_shifts,
                args.iq_blocks,
                "i-turn",
            )
            branch.q = turning_point_scan_and_apply(
                session,
                "mitp",
                "q",
                branch.q,
                iq_small_window,
                args.iq_turn_window,
                args.small_step,
                args.iq_turn_step,
                args.iq_turn_max_shifts,
                args.iq_blocks,
                "q-turn",
            )
        if args.disable_p_turn_search:
            branch.p = scan_and_apply(session, "qtp", "p", branch.p, p_small_window, args.small_step, args.p_blocks)
        else:
            branch.p = turning_point_scan_and_apply(
                session,
                "qtp",
                "p",
                branch.p,
                p_small_window,
                args.p_turn_window,
                args.small_step,
                args.p_turn_step,
                args.p_turn_max_shifts,
                args.p_blocks,
                "p-turn",
            )

        if not args.disable_prelock_iq_recheck:
            session.progress(
                "pre-lock I/Q turning recheck: "
                f"turn-step={args.iq_turn_step:.3f}V "
                f"confirm +/-{args.prelock_iq_recheck_window:.3f}V "
                f"step={args.prelock_iq_recheck_step:.3f}V"
            )
            branch.i = turning_point_scan_and_apply(
                session,
                "mitp",
                "i",
                branch.i,
                args.prelock_iq_recheck_window,
                args.prelock_iq_recheck_window,
                args.prelock_iq_recheck_step,
                args.iq_turn_step,
                args.iq_turn_max_shifts,
                args.iq_blocks,
                "pre-lock I recheck",
            )
            branch.q = turning_point_scan_and_apply(
                session,
                "mitp",
                "q",
                branch.q,
                args.prelock_iq_recheck_window,
                args.prelock_iq_recheck_window,
                args.prelock_iq_recheck_step,
                args.iq_turn_step,
                args.iq_turn_max_shifts,
                args.iq_blocks,
                "pre-lock Q recheck",
            )

        anchor_baseline: dict[str, ProbeMetric] | None = None
        if args.anchor_guard_duration > 0.0:
            if args.no_lock_at_end:
                session.progress("anchor guard requested but --no-lock-at-end is set; guard disabled")
            else:
                session.progress("anchor guard baseline capture before lock start")
                anchor_baseline = refresh_iq_anchor_baseline(session, "anchor guard baseline")
                off_axes = [
                    axis for axis in ("i", "q")
                    if not anchor_direction_valid(anchor_baseline[axis])
                ]
                if off_axes:
                    session.progress(
                        "anchor guard pre-lock baseline needs recheck: " +
                        ",".join(axis.upper() for axis in off_axes)
                    )
                    anchor_baseline = recheck_anchor_axes(
                        session,
                        branch,
                        args,
                        off_axes,
                        "anchor pre-lock",
                    )

        if not args.no_lock_at_end:
            session.progress("starting closed-loop bias control")
            session.send("dpmzm lock start")
            session.read_for(1.0)
            session.send("dpmzm lock status")
            session.read_for(2.0)
            if anchor_baseline is not None:
                run_anchor_guard(session, branch, args, anchor_baseline)

        session.send("dpmzm status")
        session.read_for(2.0)
        if args.pilot_off_at_end:
            if not args.no_lock_at_end:
                session.progress("pilot-off-at-end requested; stopping lock before turning pilot off")
                session.send("dpmzm lock stop")
                session.read_for(1.0)
            session.send("dpmzm set pilot-open off")
            session.read_for(1.0)

        save_metrics(metrics_path, session.metric_rows)
        session.progress(
            f"final positive-branch point: I={branch.i:+.3f}V Q={branch.q:+.3f}V P={branch.p:+.3f}V"
        )
        session.progress(f"serial log saved: {log_path}")
        session.progress(f"metrics csv saved: {metrics_path}")
        if not args.no_plot:
            try:
                from plot_dpmzm_flow_metrics import plot_flow_metrics

                image_path = args.plot_dir / f"{stem}_all_stages.png"
                plot_flow_metrics(
                    metrics_path,
                    out_path=image_path,
                    title=f"DPMZM positive-branch flow ({stamp})",
                )
                session.progress(f"all-stages plot saved: {image_path}")
            except Exception as exc:
                session.progress(f"plot failed: {exc!r}")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
