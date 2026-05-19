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

import matplotlib.pyplot as plt


RAW_DATA_DIR = Path(r"C:\Users\Administrator\Desktop\DPMZM_contral_bais\raw data")
PLOT_DIR = Path(r"C:\Users\Administrator\Desktop\DPMZM_contral_bais\simulation_image\lock_firmware")

RE_CYCLE = re.compile(r"\[dpmzm\]\[lock\]\s+cycle\s+axis=([iqp])", re.I)
RE_CENTER = re.compile(
    r"center:\s+([+-]?\d+\.\d+)V\s+plus=([+-]?\d+\.\d+)V\s+minus=([+-]?\d+\.\d+)V"
)
RE_METRIC0 = re.compile(
    r"metric0:\s+([+-]?\d+\.\d+)\s+\(([+-]?\d+\.\d+)\s+dBm\)\s+dc=([+-]?\d+\.\d+)V\s+best=(yes|no)",
    re.I,
)
RE_METRIC_PLUS = re.compile(
    r"metric\+:\s+([+-]?\d+\.\d+)\s+\(([+-]?\d+\.\d+)\s+dBm\)\s+dc=([+-]?\d+\.\d+)V",
    re.I,
)
RE_METRIC_MINUS = re.compile(
    r"metric-:\s+([+-]?\d+\.\d+)\s+\(([+-]?\d+\.\d+)\s+dBm\)\s+dc=([+-]?\d+\.\d+)V",
    re.I,
)
RE_ERROR = re.compile(r"e:\s+([+-]?\d+\.\d+)\s+direction=([A-Za-z0-9_-]+)", re.I)
RE_STEP = re.compile(
    r"step\s+axis=([iqp])\s+requested=([+-]?\d+\.\d+)V\s+applied=([+-]?\d+\.\d+)V\s+new=([+-]?\d+\.\d+)V\s+hold=(yes|no)\s+clamp=(yes|no)",
    re.I,
)
RE_BIAS_STATUS = re.compile(
    r"bias target:\s+I=([+-]?\d+\.\d+)V\s+Q=([+-]?\d+\.\d+)V\s+P=([+-]?\d+\.\d+)V",
    re.I,
)


@dataclass
class LockRecord:
    t_s: float
    axis: str
    bias_i: float
    bias_q: float
    bias_p: float
    center_v: float
    plus_v: float
    minus_v: float
    metric0: float
    metric_plus: float
    metric_minus: float
    dbm0: float
    dbm_plus: float
    dbm_minus: float
    dc0: float
    dc_plus: float
    dc_minus: float
    center_is_best: bool
    error: float
    direction: str
    requested_step_v: float
    applied_step_v: float
    new_bias_v: float
    hold: bool
    clamp: bool


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Capture firmware DPMZM closed-loop step logs and plot lock drift."
    )
    parser.add_argument("--port", default="COM8")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--duration", type=float, default=300.0, help="Capture duration in seconds.")
    parser.add_argument("--output-dir", type=Path, default=RAW_DATA_DIR)
    parser.add_argument("--plot-dir", type=Path, default=PLOT_DIR)
    parser.add_argument("--start-lock", action="store_true", help="Send 'dpmzm lock start' before capture.")
    parser.add_argument("--no-setup", action="store_true", help="Do not send status/dump/pilot setup commands.")
    parser.add_argument("--poll-status", type=float, default=30.0, help="Send 'dpmzm status' every N seconds; 0 disables.")
    parser.add_argument("--no-plot", action="store_true")
    return parser.parse_args()


def write_line(path: Path, text: str) -> None:
    with path.open("a", encoding="utf-8") as f:
        f.write(text)


def send_command(ser: serial.Serial, log_path: Path, cmd: str) -> None:
    line = f"\n>>> {cmd}\n"
    sys.stdout.write(line)
    write_line(log_path, line)
    ser.write((cmd + "\r\n").encode("ascii", errors="ignore"))
    ser.flush()


def update_bias_estimate(axis: str, new_v: float, bias_i: float, bias_q: float, bias_p: float) -> tuple[float, float, float]:
    if axis == "i":
        bias_i = new_v
    elif axis == "q":
        bias_q = new_v
    elif axis == "p":
        bias_p = new_v
    return bias_i, bias_q, bias_p


def maybe_float(value: str | None) -> float:
    if value is None:
        return float("nan")
    return float(value)


def try_build_record(
    step_line: str,
    current: dict[str, object],
    t_s: float,
    bias_i: float,
    bias_q: float,
    bias_p: float,
) -> LockRecord | None:
    m_step = RE_STEP.search(step_line)
    if not m_step:
        return None
    required = ["center", "metric0", "metric_plus", "metric_minus", "error"]
    if any(k not in current for k in required):
        return None

    axis = m_step.group(1).lower()
    center = current["center"]
    metric0 = current["metric0"]
    metric_plus = current["metric_plus"]
    metric_minus = current["metric_minus"]
    err = current["error"]

    return LockRecord(
        t_s=t_s,
        axis=axis,
        bias_i=bias_i,
        bias_q=bias_q,
        bias_p=bias_p,
        center_v=center[0],
        plus_v=center[1],
        minus_v=center[2],
        metric0=metric0[0],
        metric_plus=metric_plus[0],
        metric_minus=metric_minus[0],
        dbm0=metric0[1],
        dbm_plus=metric_plus[1],
        dbm_minus=metric_minus[1],
        dc0=metric0[2],
        dc_plus=metric_plus[2],
        dc_minus=metric_minus[2],
        center_is_best=metric0[3],
        error=err[0],
        direction=err[1],
        requested_step_v=float(m_step.group(2)),
        applied_step_v=float(m_step.group(3)),
        new_bias_v=float(m_step.group(4)),
        hold=m_step.group(5).lower() == "yes",
        clamp=m_step.group(6).lower() == "yes",
    )


def save_csv(path: Path, records: list[LockRecord]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow([
            "t_s",
            "axis",
            "bias_i",
            "bias_q",
            "bias_p",
            "center_v",
            "plus_v",
            "minus_v",
            "metric0",
            "metric_plus",
            "metric_minus",
            "dbm0",
            "dbm_plus",
            "dbm_minus",
            "dc0",
            "dc_plus",
            "dc_minus",
            "center_is_best",
            "error",
            "direction",
            "requested_step_v",
            "applied_step_v",
            "new_bias_v",
            "hold",
            "clamp",
        ])
        for r in records:
            writer.writerow([
                f"{r.t_s:.3f}",
                r.axis,
                f"{r.bias_i:.6f}",
                f"{r.bias_q:.6f}",
                f"{r.bias_p:.6f}",
                f"{r.center_v:.6f}",
                f"{r.plus_v:.6f}",
                f"{r.minus_v:.6f}",
                f"{r.metric0:.9f}",
                f"{r.metric_plus:.9f}",
                f"{r.metric_minus:.9f}",
                f"{r.dbm0:.3f}",
                f"{r.dbm_plus:.3f}",
                f"{r.dbm_minus:.3f}",
                f"{r.dc0:.6f}",
                f"{r.dc_plus:.6f}",
                f"{r.dc_minus:.6f}",
                int(r.center_is_best),
                f"{r.error:.9f}",
                r.direction,
                f"{r.requested_step_v:.6f}",
                f"{r.applied_step_v:.6f}",
                f"{r.new_bias_v:.6f}",
                int(r.hold),
                int(r.clamp),
            ])


def plot_records(path: Path, records: list[LockRecord], title: str) -> None:
    if not records:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    t = [r.t_s for r in records]

    fig, axes = plt.subplots(5, 1, figsize=(15, 13), sharex=True)
    axes[0].plot(t, [r.bias_i for r in records], label="I bias")
    axes[0].plot(t, [r.bias_q for r in records], label="Q bias")
    axes[0].plot(t, [r.bias_p for r in records], label="P bias")
    axes[0].set_ylabel("Bias (V)")
    axes[0].legend()
    axes[0].grid(True, alpha=0.3)

    for axis, marker in [("i", "o"), ("q", "s"), ("p", "^")]:
        xs = [r.t_s for r in records if r.axis == axis]
        ys = [r.dbm0 for r in records if r.axis == axis]
        axes[1].plot(xs, ys, marker=marker, label=f"{axis.upper()} metric")
    axes[1].set_ylabel("Center metric (dBm)")
    axes[1].legend()
    axes[1].grid(True, alpha=0.3)

    for axis, marker in [("i", "o"), ("q", "s"), ("p", "^")]:
        xs = [r.t_s for r in records if r.axis == axis]
        ys = [r.dc0 for r in records if r.axis == axis]
        axes[2].plot(xs, ys, marker=marker, label=f"{axis.upper()} DC")
    axes[2].set_ylabel("DC (V)")
    axes[2].legend()
    axes[2].grid(True, alpha=0.3)

    axes[3].plot(t, [r.error for r in records], marker="o", label="normalized error")
    axes[3].axhline(0.0, color="0.4", linewidth=1)
    axes[3].set_ylabel("Error")
    axes[3].legend()
    axes[3].grid(True, alpha=0.3)

    axes[4].plot(t, [r.applied_step_v * 1000.0 for r in records], marker="o", label="applied step")
    axes[4].axhline(0.0, color="0.4", linewidth=1)
    axes[4].set_ylabel("Step (mV)")
    axes[4].set_xlabel("Time (s)")
    axes[4].legend()
    axes[4].grid(True, alpha=0.3)

    fig.suptitle(title)
    fig.tight_layout(rect=(0, 0, 1, 0.97))
    fig.savefig(path, dpi=150)
    plt.close(fig)


def main() -> int:
    args = parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    args.plot_dir.mkdir(parents=True, exist_ok=True)

    stamp = datetime.now().strftime("%Y-%m-%d_%H%M%S")
    log_path = args.output_dir / f"{stamp}_dpmzm_firmware_lock_capture_serial.log"
    csv_path = args.output_dir / f"{stamp}_dpmzm_firmware_lock_capture_steps.csv"
    plot_path = args.plot_dir / f"{stamp}_dpmzm_firmware_lock_capture.png"

    records: list[LockRecord] = []
    current: dict[str, object] = {}
    bias_i = float("nan")
    bias_q = float("nan")
    bias_p = float("nan")

    try:
        with serial.Serial(args.port, baudrate=args.baud, timeout=0.05, write_timeout=2) as ser:
            start = time.time()
            next_status = start
            buffer = ""

            if not args.no_setup:
                send_command(ser, log_path, "dpmzm set dump metrics")
                time.sleep(0.5)
                send_command(ser, log_path, "dpmzm set pilot-open on")
                time.sleep(0.5)
                send_command(ser, log_path, "dpmzm status")
                time.sleep(0.8)
            if args.start_lock:
                send_command(ser, log_path, "dpmzm lock start")
                time.sleep(0.5)
            send_command(ser, log_path, "dpmzm lock status")

            while time.time() - start < args.duration:
                now = time.time()
                if args.poll_status > 0 and now >= next_status:
                    send_command(ser, log_path, "dpmzm status")
                    next_status = now + args.poll_status

                payload = ser.read(4096)
                if not payload:
                    time.sleep(0.02)
                    continue
                text = payload.decode("utf-8", errors="replace")
                sys.stdout.write(text)
                sys.stdout.flush()
                write_line(log_path, text)
                buffer += text
                while "\n" in buffer:
                    line, buffer = buffer.split("\n", 1)
                    clean = line.strip()
                    if not clean:
                        continue

                    if m := RE_BIAS_STATUS.search(clean):
                        bias_i = float(m.group(1))
                        bias_q = float(m.group(2))
                        bias_p = float(m.group(3))
                    elif m := RE_CYCLE.search(clean):
                        current = {"axis": m.group(1).lower()}
                    elif m := RE_CENTER.search(clean):
                        current["center"] = (float(m.group(1)), float(m.group(2)), float(m.group(3)))
                    elif m := RE_METRIC0.search(clean):
                        current["metric0"] = (
                            float(m.group(1)),
                            float(m.group(2)),
                            float(m.group(3)),
                            m.group(4).lower() == "yes",
                        )
                    elif m := RE_METRIC_PLUS.search(clean):
                        current["metric_plus"] = (float(m.group(1)), float(m.group(2)), float(m.group(3)))
                    elif m := RE_METRIC_MINUS.search(clean):
                        current["metric_minus"] = (float(m.group(1)), float(m.group(2)), float(m.group(3)))
                    elif m := RE_ERROR.search(clean):
                        current["error"] = (float(m.group(1)), m.group(2))
                    elif RE_STEP.search(clean):
                        rec = try_build_record(clean, current, time.time() - start, bias_i, bias_q, bias_p)
                        if rec is not None:
                            records.append(rec)
                            bias_i, bias_q, bias_p = update_bias_estimate(
                                rec.axis, rec.new_bias_v, bias_i, bias_q, bias_p
                            )

        save_csv(csv_path, records)
        if not args.no_plot:
            plot_records(plot_path, records, f"DPMZM firmware lock capture ({stamp})")
    except serial.SerialException as exc:
        print(f"Serial error on {args.port}: {exc}", file=sys.stderr)
        return 10

    print(f"SERIAL_LOG={log_path}")
    print(f"LOCK_CSV={csv_path}")
    if not args.no_plot:
        print(f"PLOT={plot_path}")
    print(f"RECORDS={len(records)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
