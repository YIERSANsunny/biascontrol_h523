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
METRIC_PREFIX = "DPMZMCSV,"

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

    def wait_scan_done(self, timeout_s: float, label: str) -> float:
        start = time.time()
        seen = len(self.lines)
        last_progress = start
        best_pattern = re.compile(r"\[dpmzm\]\s+scan done: best .* at ([+-]?\d+\.\d+) V")

        while time.time() - start < timeout_s:
            self.read_for(0.25)
            for line in self.lines[seen:]:
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
    parser.add_argument("--startup-wait", type=float, default=0.8)
    parser.add_argument("--coarse-timeout", type=float, default=420.0)
    parser.add_argument("--fine-timeout", type=float, default=780.0)
    parser.add_argument("--small-window", type=float, default=0.30)
    parser.add_argument("--small-step", type=float, default=0.01)
    parser.add_argument("--blocks", type=int, default=10)
    parser.add_argument("--initial-i", type=float, default=0.0)
    parser.add_argument("--initial-q", type=float, default=0.0)
    parser.add_argument("--initial-p", type=float, default=0.0)
    parser.add_argument(
        "--skip-initial-bias",
        action="store_true",
        help="Start auto coarse from the board's current I/Q/P biases instead of the default 0/0/0 V.",
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
    return parser.parse_args()


def clamp(value: float, lo: float = -9.0, hi: float = 9.0) -> float:
    return max(lo, min(hi, value))


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
            if state == "FINE_SCAN_P_FINE" and stage == "qtp" and target == "p":
                picks.positive_p = best
            elif state == "FINE_SCAN_I_FINE" and stage == "mitp" and target == "i":
                picks.i = best
            elif state == "FINE_SCAN_Q_FINE" and stage == "mitp" and target == "q":
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
    best = session.wait_scan_done(timeout_s=180.0, label=label)
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

        session.send("dpmzm auto coarse")
        coarse_line = session.wait_for_any(
            ["[dpmzm][auto] applied coarse result", "[dpmzm][auto] coarse failed", "[dpmzm][auto] failed"],
            args.coarse_timeout,
            "auto coarse",
        )
        if coarse_line is None or "failed" in coarse_line:
            return 2

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

        branch.p = scan_and_apply(session, "qtp", "p", branch.p, args.small_window, args.small_step, args.blocks)
        branch.i = scan_and_apply(session, "mitp", "i", branch.i, args.small_window, args.small_step, args.blocks)
        branch.q = scan_and_apply(session, "mitp", "q", branch.q, args.small_window, args.small_step, args.blocks)
        branch.p = scan_and_apply(session, "qtp", "p", branch.p, args.small_window, args.small_step, args.blocks)

        if not args.no_lock_at_end:
            session.progress("starting closed-loop bias control")
            session.send("dpmzm lock start")
            session.read_for(1.0)
            session.send("dpmzm lock status")
            session.read_for(2.0)

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
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
