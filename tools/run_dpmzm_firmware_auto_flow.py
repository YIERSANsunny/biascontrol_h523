# -*- coding: utf-8 -*-

from __future__ import annotations

import argparse
import csv
import sys
import time
from datetime import datetime
from pathlib import Path

try:
    import serial
except ImportError as exc:
    raise SystemExit(
        "pyserial is required. Install it first, for example: pip install pyserial"
    ) from exc

from plot_dpmzm_flow_metrics import plot_flow_metrics


RAW_DATA_DIR = Path(r"C:\Users\Administrator\Desktop\DPMZM_contral_bais\raw data")
SIM_IMAGE_DIR = Path(r"C:\Users\Administrator\Desktop\DPMZM_contral_bais\simulation_image\auto_flow")
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
        self.log(line)

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

    def flush_buffer(self) -> None:
        if self.buffer.strip():
            self._record_line(self.buffer)
        self.buffer = ""

    def send(self, command: str) -> None:
        self.progress(f">>> {command}")
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
                        self.progress(f"{label} matched: {line}")
                        return line
            seen = len(self.lines)
            now = time.time()
            if now - last_progress >= 10.0:
                self.progress(f"{label} still running, elapsed {now - start:.0f}s")
                last_progress = now
        self.progress(f"{label} timeout after {timeout_s:.0f}s")
        return None


def write_metrics_csv(csv_path: Path, rows: list[list[str]]) -> None:
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    with csv_path.open("w", newline="", encoding="utf-8") as file_obj:
        writer = csv.writer(file_obj)
        writer.writerow(METRIC_HEADER)
        writer.writerows(rows)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run the pure firmware DPMZM auto flow over UART: "
            "auto coarse -> auto fine -> optional lock start."
        )
    )
    parser.add_argument("--port", default="COM8", help="Serial port, for example COM8.")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate.")
    parser.add_argument("--output-dir", type=Path, default=RAW_DATA_DIR, help="Raw log/CSV directory.")
    parser.add_argument("--plot-dir", type=Path, default=SIM_IMAGE_DIR, help="Output plot directory.")
    parser.add_argument("--startup-wait", type=float, default=0.5, help="Initial serial drain time in seconds.")
    parser.add_argument("--coarse-timeout", type=float, default=360.0, help="auto coarse timeout in seconds.")
    parser.add_argument("--fine-timeout", type=float, default=360.0, help="auto fine timeout in seconds.")
    parser.add_argument("--initial-i", type=float, default=0.0, help="Initial I bias before auto coarse.")
    parser.add_argument("--initial-q", type=float, default=0.0, help="Initial Q bias before auto coarse.")
    parser.add_argument("--initial-p", type=float, default=0.0, help="Initial P bias before auto coarse.")
    parser.add_argument(
        "--skip-initial-bias",
        action="store_true",
        help="Do not set I/Q/P before auto coarse; use the board's current bias.",
    )
    parser.add_argument(
        "--no-lock-at-end",
        action="store_true",
        help="Stop after auto fine instead of starting closed-loop lock.",
    )
    parser.add_argument("--no-plot", action="store_true", help="Do not generate the all-stages PNG.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    args.plot_dir.mkdir(parents=True, exist_ok=True)

    stamp = datetime.now().strftime("%Y-%m-%d_%H%M%S")
    log_path = args.output_dir / f"{stamp}_dpmzm_firmware_auto_flow_serial.log"
    progress_path = args.output_dir / f"{stamp}_dpmzm_firmware_auto_flow_progress.log"
    csv_path = args.output_dir / f"{stamp}_dpmzm_firmware_auto_flow_metrics.csv"

    exit_code = 0
    image_path: Path | None = None

    try:
        with serial.Serial(args.port, baudrate=args.baud, timeout=0.05, write_timeout=2) as ser:
            session = SerialSession(ser, log_path, progress_path)
            session.progress(f"log: {log_path}")
            session.read_for(args.startup_wait)

            setup_commands: list[tuple[str, float]] = [
                ("dpmzm lock stop", 0.8),
                ("dpmzm set dump metrics", 0.8),
                ("dpmzm set pilot-open on", 1.0),
            ]
            if not args.skip_initial_bias:
                setup_commands.extend(
                    [
                        (f"dpmzm set bias i {args.initial_i:.3f}", 0.8),
                        (f"dpmzm set bias q {args.initial_q:.3f}", 0.8),
                        (f"dpmzm set bias p {args.initial_p:.3f}", 0.8),
                    ]
                )
            setup_commands.append(("dpmzm status", 1.5))

            for command, wait_s in setup_commands:
                session.send(command)
                session.read_for(wait_s)

            session.send("dpmzm auto coarse")
            coarse_line = session.wait_for_any(
                [
                    "[dpmzm][auto] applied coarse result",
                    "[dpmzm][auto] coarse failed",
                    "[dpmzm][auto] failed",
                ],
                args.coarse_timeout,
                "auto coarse",
            )
            if coarse_line is None or "failed" in coarse_line:
                exit_code = 2
            else:
                session.send("dpmzm auto fine")
                fine_line = session.wait_for_any(
                    ["[dpmzm][auto] applied fine result", "[dpmzm][auto] fine failed"],
                    args.fine_timeout,
                    "auto fine",
                )
                if fine_line is None or "failed" in fine_line:
                    exit_code = 3

            if exit_code == 0 and not args.no_lock_at_end:
                session.progress("starting closed-loop bias control")
                session.send("dpmzm lock start")
                session.read_for(1.0)
                session.send("dpmzm lock status")
                session.read_for(2.0)
                session.send("dpmzm status")
                session.read_for(1.5)

            session.flush_buffer()
            write_metrics_csv(csv_path, session.metric_rows)
            session.progress(f"serial log saved: {log_path}")
            session.progress(f"progress log saved: {progress_path}")
            session.progress(f"metrics csv saved: {csv_path}")

    except serial.SerialException as exc:
        print(f"Serial error on {args.port}: {exc}", file=sys.stderr)
        return 10

    if exit_code == 0 and not args.no_plot:
        try:
            image_path = plot_flow_metrics(
                csv_path,
                out_dir=args.plot_dir,
                title=f"DPMZM firmware auto flow ({stamp})",
            )
            print(f"all-stages plot saved: {image_path}")
        except Exception as exc:  # Keep the scan result even if plotting fails.
            print(f"plot failed: {exc}", file=sys.stderr)
            exit_code = 4

    print(f"SERIAL_LOG={log_path}")
    print(f"PROGRESS_LOG={progress_path}")
    print(f"METRICS_CSV={csv_path}")
    if image_path is not None:
        print(f"PLOT={image_path}")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
