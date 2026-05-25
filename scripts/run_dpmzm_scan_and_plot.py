# -*- coding: utf-8 -*-

from __future__ import annotations

import argparse
import csv
import re
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
SIM_IMAGE_DIR = Path(r"C:\Users\Administrator\Desktop\DPMZM_contral_bais\simulation_image\manual_scan")

METRIC_PREFIX = "DPMZMCSV,"
SUMMARY_PREFIX = "DPMZMSUM,"

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


class SerialScanSession:
    def __init__(self, ser: serial.Serial, log_path: Path, progress_path: Path):
        self.ser = ser
        self.log_path = log_path
        self.progress_path = progress_path
        self.buffer = ""
        self.lines: list[str] = []
        self.metric_rows: list[list[str]] = []
        self.summary_lines: list[str] = []

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
            fields = [field.strip() for field in clean[len(METRIC_PREFIX) :].split(",")]
            if len(fields) == len(METRIC_HEADER):
                self.metric_rows.append(fields)
        elif clean.startswith(SUMMARY_PREFIX):
            self.summary_lines.append(clean)

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

    def wait_for_scan_done(
        self,
        timeout_s: float,
        progress_interval_s: float,
        debug_interval_s: float,
    ) -> bool:
        start = time.time()
        last_progress = start
        last_debug = start
        seen = len(self.lines)
        while time.time() - start < timeout_s:
            self.read_for(0.25)
            new_lines = self.lines[seen:]
            seen = len(self.lines)
            for line in new_lines:
                lowered = line.lower()
                if line.startswith(SUMMARY_PREFIX) or "[dpmzm] scan done" in lowered:
                    self.progress(f"scan completed: {line}")
                    return True
                if "scan failed" in lowered or "last_error=" in lowered and "none" not in lowered:
                    self.progress(f"scan reported an error: {line}")
                    return False

            now = time.time()
            if progress_interval_s > 0 and now - last_progress >= progress_interval_s:
                self.progress(f"scan still running, elapsed {now - start:.0f}s")
                last_progress = now
            if debug_interval_s > 0 and now - last_debug >= debug_interval_s:
                self.send("dpmzm debug")
                self.read_for(0.8)
                last_debug = now

        self.progress(f"scan timeout after {timeout_s:.0f}s")
        return False


def write_metrics_csv(csv_path: Path, rows: list[list[str]]) -> None:
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    with csv_path.open("w", newline="", encoding="utf-8") as file_obj:
        writer = csv.writer(file_obj)
        writer.writerow(METRIC_HEADER)
        writer.writerows(rows)


def make_safe_slug(text: str, max_len: int = 90) -> str:
    text = text.strip().lower()
    text = re.sub(r"^dpmzm\s+", "", text)
    text = re.sub(r"[^a-z0-9.+-]+", "_", text)
    text = text.strip("_")
    text = text.replace(".", "p")
    return (text[:max_len].strip("_") or "scan")


def normalize_scan_command(command: str) -> str:
    command = command.strip()
    if command.lower().startswith("dpmzm scan "):
        return command
    if command.lower().startswith("scan "):
        return f"dpmzm {command}"
    return command


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run one DPMZM scan over UART, save the raw serial log, extract DPMZMCSV "
            "metrics, and plot the scan curve."
        )
    )
    parser.add_argument(
        "--scan",
        required=True,
        help='Scan command, e.g. "dpmzm scan matp i -9.0 9.0 0.5 4".',
    )
    parser.add_argument("--port", default="COM9", help="Serial port, for example COM9.")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate.")
    parser.add_argument("--timeout", type=float, default=900.0, help="Scan timeout in seconds.")
    parser.add_argument("--startup-wait", type=float, default=0.5, help="Initial serial drain time.")
    parser.add_argument("--output-dir", type=Path, default=RAW_DATA_DIR, help="Raw log/CSV directory.")
    parser.add_argument("--plot-dir", type=Path, default=SIM_IMAGE_DIR, help="Output image directory.")
    parser.add_argument("--no-plot", action="store_true", help="Only save log/CSV; do not plot.")
    parser.add_argument("--no-mode-dpmzm", action="store_true", help="Do not send mode dpmzm before scan.")
    parser.add_argument("--no-pilot-open", action="store_true", help="Do not force continuous onboard pilot.")
    parser.add_argument("--no-dump-metrics", action="store_true", help="Do not send dpmzm set dump metrics.")
    parser.add_argument("--no-lock-stop", action="store_true", help="Do not stop DPMZM lock before scanning.")
    parser.add_argument("--bias-i", type=float, default=None, help="Optional I bias to set before scan.")
    parser.add_argument("--bias-q", type=float, default=None, help="Optional Q bias to set before scan.")
    parser.add_argument("--bias-p", type=float, default=None, help="Optional P bias to set before scan.")
    parser.add_argument(
        "--progress-interval",
        type=float,
        default=15.0,
        help="Print a progress line every N seconds while scanning. Use 0 to disable.",
    )
    parser.add_argument(
        "--debug-interval",
        type=float,
        default=0.0,
        help="Send dpmzm debug every N seconds while scanning. Use 0 to disable.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    args.plot_dir.mkdir(parents=True, exist_ok=True)

    scan_command = normalize_scan_command(args.scan)
    stamp = datetime.now().strftime("%Y-%m-%d_%H%M%S")
    slug = make_safe_slug(scan_command)
    log_path = args.output_dir / f"{stamp}_dpmzm_{slug}_serial.log"
    progress_path = args.output_dir / f"{stamp}_dpmzm_{slug}_progress.log"
    csv_path = args.output_dir / f"{stamp}_dpmzm_{slug}_metrics.csv"
    summary_path = args.output_dir / f"{stamp}_dpmzm_{slug}_summary.txt"

    try:
        with serial.Serial(args.port, baudrate=args.baud, timeout=0.05, write_timeout=2) as ser:
            session = SerialScanSession(ser, log_path, progress_path)
            session.progress(f"serial port: {args.port} @ {args.baud}")
            session.progress(f"scan command: {scan_command}")
            session.progress(f"serial log: {log_path}")
            session.read_for(args.startup_wait)

            setup_commands: list[tuple[str, float]] = []
            if not args.no_lock_stop:
                setup_commands.append(("dpmzm lock stop", 0.8))
            if not args.no_mode_dpmzm:
                setup_commands.append(("mode dpmzm", 0.8))
            if not args.no_dump_metrics:
                setup_commands.append(("dpmzm set dump metrics", 0.8))
            if not args.no_pilot_open:
                setup_commands.append(("dpmzm set pilot-open on", 1.0))
            for axis, value in (("i", args.bias_i), ("q", args.bias_q), ("p", args.bias_p)):
                if value is not None:
                    setup_commands.append((f"dpmzm set bias {axis} {value:.6f}", 0.8))

            for command, wait_s in setup_commands:
                session.send(command)
                session.read_for(wait_s)

            session.send(scan_command)
            ok = session.wait_for_scan_done(
                timeout_s=args.timeout,
                progress_interval_s=args.progress_interval,
                debug_interval_s=args.debug_interval,
            )
            session.flush_buffer()

        write_metrics_csv(csv_path, session.metric_rows)

        with summary_path.open("w", encoding="utf-8") as file_obj:
            file_obj.write(f"scan command: {scan_command}\n")
            file_obj.write(f"port: {args.port}\n")
            file_obj.write(f"metric rows: {len(session.metric_rows)}\n")
            file_obj.write(f"scan completed: {ok}\n")
            file_obj.write(f"serial log: {log_path}\n")
            file_obj.write(f"progress log: {progress_path}\n")
            file_obj.write(f"metrics csv: {csv_path}\n")
            if session.summary_lines:
                file_obj.write("\nDPMZMSUM:\n")
                for line in session.summary_lines:
                    file_obj.write(line + "\n")

        print(f"serial log saved: {log_path}")
        print(f"progress log saved: {progress_path}")
        print(f"metrics csv saved: {csv_path}")
        print(f"summary saved: {summary_path}")

        if not args.no_plot:
            image_path = plot_flow_metrics(
                csv_path,
                out_dir=args.plot_dir,
                title=f"DPMZM scan: {scan_command} ({stamp})",
            )
            print(f"plot saved: {image_path}")

        return 0 if ok and session.metric_rows else 2
    except serial.SerialException as exc:
        print(f"Serial error on {args.port}: {exc}", file=sys.stderr)
        return 10


if __name__ == "__main__":
    raise SystemExit(main())
