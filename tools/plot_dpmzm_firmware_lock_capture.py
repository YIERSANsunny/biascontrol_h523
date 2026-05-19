# -*- coding: utf-8 -*-

from __future__ import annotations

import argparse
import csv
import math
import re
import statistics
from pathlib import Path

import matplotlib.pyplot as plt


RAW_DATA_DIR = Path(r"C:\Users\Administrator\Desktop\DPMZM_contral_bais\raw data")
PLOT_DIR = Path(r"C:\Users\Administrator\Desktop\DPMZM_contral_bais\simulation_image\lock_firmware")

ANCHOR_PATTERNS = [
    re.compile(
        r"anchor:\s+yes\s+I=([+-]?\d+\.\d+)V\s+Q=([+-]?\d+\.\d+)V\s+P=([+-]?\d+\.\d+)V",
        re.I,
    ),
    re.compile(
        r"anchor\s+I=([+-]?\d+\.\d+)V\s+Q=([+-]?\d+\.\d+)V\s+P=([+-]?\d+\.\d+)V",
        re.I,
    ),
]

AXES = ("i", "q", "p")
AXIS_LABELS = {"i": "I", "q": "Q", "p": "P"}
COLORS = {"i": "#d62728", "q": "#2ca02c", "p": "#1f77b4"}
MARKERS = {"i": "o", "q": "s", "p": "^"}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Plot DPMZM firmware lock capture CSV files."
    )
    parser.add_argument(
        "csv",
        nargs="?",
        type=Path,
        help="Path to *_dpmzm_firmware_lock_capture_steps.csv. Defaults to latest.",
    )
    parser.add_argument("--raw-dir", type=Path, default=RAW_DATA_DIR)
    parser.add_argument("--plot-dir", type=Path, default=PLOT_DIR)
    parser.add_argument("--title", default=None)
    parser.add_argument("--no-scatter", action="store_true")
    return parser.parse_args()


def latest_steps_csv(raw_dir: Path) -> Path:
    files = sorted(
        raw_dir.glob("*_dpmzm_firmware_lock_capture_steps.csv"),
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )
    if not files:
        raise FileNotFoundError(f"No lock capture steps CSV found in {raw_dir}")
    return files[0]


def parse_bool(value: str) -> bool:
    return value.strip().lower() in {"1", "true", "yes", "y"}


def parse_float(value: str) -> float:
    value = value.strip()
    if value == "":
        return float("nan")
    try:
        return float(value)
    except ValueError:
        return float("nan")


def load_records(csv_path: Path) -> list[dict[str, object]]:
    records: list[dict[str, object]] = []
    with csv_path.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            rec: dict[str, object] = {"axis": row.get("axis", "").strip().lower()}
            for key, value in row.items():
                if key == "axis":
                    continue
                if key in {"center_is_best", "hold", "clamp"}:
                    rec[key] = parse_bool(value or "")
                else:
                    rec[key] = parse_float(value or "")
            if rec["axis"] in AXES:
                records.append(rec)
    if not records:
        raise ValueError(f"No valid lock records in {csv_path}")
    t0 = float(records[0]["t_s"])
    for rec in records:
        rec["t_rel_s"] = float(rec["t_s"]) - t0
    return records


def serial_log_for_csv(csv_path: Path) -> Path:
    name = csv_path.name.replace("_steps.csv", "_serial.log")
    return csv_path.with_name(name)


def parse_anchor(csv_path: Path) -> tuple[float, float, float] | None:
    log_path = serial_log_for_csv(csv_path)
    if not log_path.exists():
        return None
    try:
        lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return None
    for line in lines:
        for pattern in ANCHOR_PATTERNS:
            m = pattern.search(line)
            if m:
                return (float(m.group(1)), float(m.group(2)), float(m.group(3)))
    return None


def finite(values: list[float]) -> list[float]:
    return [v for v in values if math.isfinite(v)]


def mean_std(values: list[float]) -> tuple[float, float]:
    vals = finite(values)
    if not vals:
        return (float("nan"), float("nan"))
    if len(vals) == 1:
        return (vals[0], 0.0)
    return (statistics.mean(vals), statistics.pstdev(vals))


def axis_records(records: list[dict[str, object]], axis: str) -> list[dict[str, object]]:
    return [r for r in records if r["axis"] == axis]


def summarize(records: list[dict[str, object]], anchor: tuple[float, float, float] | None) -> str:
    duration = max(float(r["t_rel_s"]) for r in records) if records else 0.0
    lines: list[str] = []
    lines.append(f"records: {len(records)}")
    lines.append(f"duration_s: {duration:.3f}")
    if anchor is not None:
        lines.append(f"anchor: I={anchor[0]:+.6f} V, Q={anchor[1]:+.6f} V, P={anchor[2]:+.6f} V")
    lines.append("")

    for axis in AXES:
        rows = axis_records(records, axis)
        if not rows:
            continue
        dbm = [float(r["dbm0"]) for r in rows]
        metric = [float(r["metric0"]) for r in rows]
        dc = [float(r["dc0"]) for r in rows]
        err = [float(r["error"]) for r in rows]
        steps_mv = [float(r["applied_step_v"]) * 1000.0 for r in rows]
        center = [float(r["center_v"]) for r in rows]
        holds = sum(1 for r in rows if bool(r["hold"]))
        clamps = sum(1 for r in rows if bool(r["clamp"]))
        dbm_mean, dbm_std = mean_std(dbm)
        dc_mean, dc_std = mean_std(dc)
        err_mean, err_std = mean_std(err)
        step_abs_mean, step_abs_std = mean_std([abs(v) for v in steps_mv])
        drift_mv = (center[-1] - center[0]) * 1000.0
        anchor_drift = ""
        if anchor is not None:
            anchor_v = {"i": anchor[0], "q": anchor[1], "p": anchor[2]}[axis]
            anchor_drift = f", drift_from_anchor={((center[-1] - anchor_v) * 1000.0):+.3f} mV"
        lines.append(
            f"{AXIS_LABELS[axis]}: n={len(rows)}, "
            f"dbm_mean={dbm_mean:.2f}, dbm_std={dbm_std:.2f}, "
            f"dbm_min={min(dbm):.2f}, dbm_max={max(dbm):.2f}, "
            f"metric_mean={statistics.mean(metric):.9g}, "
            f"dc={dc_mean:+.6f}±{dc_std:.6f} V, "
            f"error={err_mean:+.4f}±{err_std:.4f}, "
            f"|step|={step_abs_mean:.3f}±{step_abs_std:.3f} mV, "
            f"hold_rate={holds / len(rows):.2f}, clamps={clamps}, "
            f"center_first={center[0]:+.6f} V, center_last={center[-1]:+.6f} V, "
            f"drift={drift_mv:+.3f} mV{anchor_drift}"
        )
    return "\n".join(lines)


def add_summary_box(ax, text: str) -> None:
    short_lines = []
    for line in text.splitlines():
        if line.startswith(("I:", "Q:", "P:")):
            parts = line.split(", ")
            short_lines.append(", ".join(parts[:4] + parts[-3:]))
    if short_lines:
        ax.text(
            0.01,
            0.98,
            "\n".join(short_lines),
            transform=ax.transAxes,
            va="top",
            ha="left",
            fontsize=8,
            family="monospace",
            bbox={"boxstyle": "round,pad=0.35", "facecolor": "white", "alpha": 0.75, "edgecolor": "0.8"},
        )


def plot_overview(
    records: list[dict[str, object]],
    out_path: Path,
    title: str,
    summary_text: str,
    anchor: tuple[float, float, float] | None,
) -> None:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    t = [float(r["t_rel_s"]) for r in records]

    fig, axes = plt.subplots(6, 1, figsize=(16, 15), sharex=True)

    bias_specs = [
        ("i", "bias_i", "I bias"),
        ("q", "bias_q", "Q bias"),
        ("p", "bias_p", "P bias"),
    ]
    for idx, (axis, key, label) in enumerate(bias_specs):
        y = [float(r[key]) for r in records]
        axes[0].plot(t, y, color=COLORS[axis], marker=".", linewidth=1.5, label=label)
        if anchor is not None:
            axes[0].axhline(anchor[idx], color=COLORS[axis], linestyle="--", linewidth=1.0, alpha=0.5)
    axes[0].set_ylabel("Bias target (V)")
    axes[0].legend(loc="best")
    axes[0].grid(True, alpha=0.3)

    for axis in AXES:
        rows = axis_records(records, axis)
        xs = [float(r["t_rel_s"]) for r in rows]
        ys = [float(r["dbm0"]) for r in rows]
        axes[1].plot(xs, ys, marker=MARKERS[axis], color=COLORS[axis], linewidth=1.3, label=f"{AXIS_LABELS[axis]} metric")
    axes[1].set_ylabel("Center metric (dBm)")
    axes[1].legend(loc="best")
    axes[1].grid(True, alpha=0.3)

    for axis in AXES:
        rows = axis_records(records, axis)
        xs = [float(r["t_rel_s"]) for r in rows]
        ys = [float(r["dc0"]) for r in rows]
        axes[2].plot(xs, ys, marker=MARKERS[axis], color=COLORS[axis], linewidth=1.2, label=f"{AXIS_LABELS[axis]} DC")
    axes[2].set_ylabel("DC (V)")
    axes[2].legend(loc="best")
    axes[2].grid(True, alpha=0.3)

    for axis in AXES:
        rows = axis_records(records, axis)
        xs = [float(r["t_rel_s"]) for r in rows]
        ys = [float(r["error"]) for r in rows]
        axes[3].plot(xs, ys, marker=MARKERS[axis], color=COLORS[axis], linewidth=1.2, label=f"{AXIS_LABELS[axis]} error")
    axes[3].axhline(0.0, color="0.25", linewidth=1)
    axes[3].axhline(0.03, color="0.5", linestyle="--", linewidth=0.8)
    axes[3].axhline(-0.03, color="0.5", linestyle="--", linewidth=0.8)
    axes[3].set_ylabel("Normalized error")
    axes[3].legend(loc="best")
    axes[3].grid(True, alpha=0.3)

    for axis in AXES:
        rows = axis_records(records, axis)
        xs = [float(r["t_rel_s"]) for r in rows]
        ys = [float(r["applied_step_v"]) * 1000.0 for r in rows]
        axes[4].plot(xs, ys, marker=MARKERS[axis], color=COLORS[axis], linewidth=1.2, label=f"{AXIS_LABELS[axis]} step")
    axes[4].axhline(0.0, color="0.25", linewidth=1)
    axes[4].set_ylabel("Applied step (mV)")
    axes[4].legend(loc="best")
    axes[4].grid(True, alpha=0.3)

    for axis in AXES:
        rows = axis_records(records, axis)
        xs = [float(r["t_rel_s"]) for r in rows]
        hold_y = [1.0 if bool(r["hold"]) else 0.0 for r in rows]
        clamp_y = [1.05 if bool(r["clamp"]) else math.nan for r in rows]
        axes[5].plot(xs, hold_y, marker=MARKERS[axis], linestyle="-", color=COLORS[axis], linewidth=1.0, label=f"{AXIS_LABELS[axis]} hold")
        axes[5].plot(xs, clamp_y, marker="x", linestyle="None", color=COLORS[axis], label=f"{AXIS_LABELS[axis]} clamp")
    axes[5].set_ylim(-0.1, 1.2)
    axes[5].set_ylabel("Hold / clamp")
    axes[5].set_xlabel("Time from first record (s)")
    axes[5].legend(loc="best", ncol=3)
    axes[5].grid(True, alpha=0.3)

    fig.suptitle(title)
    fig.tight_layout(rect=(0, 0, 1, 0.975))
    fig.savefig(out_path, dpi=150)
    plt.close(fig)


def plot_metric_vs_bias(records: list[dict[str, object]], out_path: Path, title: str) -> None:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig, axes = plt.subplots(1, 3, figsize=(16, 4.5))
    for ax, axis in zip(axes, AXES):
        rows = axis_records(records, axis)
        xs = [float(r["center_v"]) for r in rows]
        ys = [float(r["dbm0"]) for r in rows]
        t = [float(r["t_rel_s"]) for r in rows]
        sc = ax.scatter(xs, ys, c=t, cmap="viridis", s=32, edgecolor="none")
        if len(xs) >= 2:
            ax.plot(xs, ys, color=COLORS[axis], alpha=0.35, linewidth=1)
        ax.set_title(f"{AXIS_LABELS[axis]} metric vs bias")
        ax.set_xlabel(f"{AXIS_LABELS[axis]} center bias (V)")
        ax.set_ylabel("Metric (dBm)")
        ax.grid(True, alpha=0.3)
    cbar = fig.colorbar(sc, ax=axes, shrink=0.9)
    cbar.set_label("Time from first record (s)")
    fig.suptitle(title)
    fig.tight_layout(rect=(0, 0, 1, 0.93))
    fig.savefig(out_path, dpi=150)
    plt.close(fig)


def main() -> int:
    args = parse_args()
    csv_path = args.csv if args.csv is not None else latest_steps_csv(args.raw_dir)
    csv_path = csv_path.resolve()
    records = load_records(csv_path)
    anchor = parse_anchor(csv_path)

    stem = csv_path.stem.replace("_steps", "")
    title = args.title or f"DPMZM firmware lock capture ({stem})"
    args.plot_dir.mkdir(parents=True, exist_ok=True)

    overview_path = args.plot_dir / f"{stem}_overview.png"
    scatter_path = args.plot_dir / f"{stem}_metric_vs_bias.png"
    summary_path = args.plot_dir / f"{stem}_summary.txt"

    summary_text = summarize(records, anchor)
    summary_path.write_text(summary_text + "\n", encoding="utf-8")

    plot_overview(records, overview_path, title, summary_text, anchor)
    if not args.no_scatter:
        plot_metric_vs_bias(records, scatter_path, title)

    print(f"CSV={csv_path}")
    print(f"SUMMARY={summary_path}")
    print(f"OVERVIEW={overview_path}")
    if not args.no_scatter:
        print(f"SCATTER={scatter_path}")
    print(summary_text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
