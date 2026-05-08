# -*- coding: utf-8 -*-

from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path


DEFAULT_IMAGE_DIR = Path(
    r"C:\Users\Administrator\Desktop\DPMZM_contral_bais\simulation_image\auto_flow"
)

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

METRIC_MAP = {
    ("matp", "i"): ("mag_fI", "MATP-I fI 1000 Hz"),
    ("matp", "q"): ("mag_fQ", "MATP-Q fQ 1200 Hz"),
    ("mitp", "i"): ("mag_fI", "MITP-I fI 1000 Hz"),
    ("mitp", "q"): ("mag_fQ", "MITP-Q fQ 1200 Hz"),
    ("qtp", "p"): ("mag_fsum_2200", "QTP-P fI+fQ 2200 Hz"),
}


def mag_to_dbm(mag_vpeak: float) -> float:
    mag = max(abs(mag_vpeak), 1e-12)
    return 10.0 * math.log10(((mag * mag) / 50.0) / 1e-3)


def load_metric_rows(csv_path: Path) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    with csv_path.open(newline="", encoding="utf-8") as file_obj:
        reader = csv.DictReader(file_obj)
        missing = [name for name in METRIC_HEADER if name not in (reader.fieldnames or [])]
        if missing:
            raise ValueError(f"{csv_path} is missing metric columns: {', '.join(missing)}")
        for raw in reader:
            row: dict[str, object] = {
                "stage": str(raw["stage"]).strip().lower(),
                "target_channel": str(raw["target_channel"]).strip().lower(),
                "sweep_unit": str(raw["sweep_unit"]).strip(),
                "pilot_mode": str(raw["pilot_mode"]).strip(),
            }
            for name in (
                "sweep_value",
                "bias_I",
                "bias_Q",
                "bias_P",
                "mag_fI",
                "mag_fQ",
                "mag_fdiff_200",
                "mag_fsum_2200",
                "dc",
            ):
                row[name] = float(raw[name])
            row["blocks"] = int(float(raw["blocks"]))
            rows.append(row)
    return rows


def split_scan_segments(rows: list[dict[str, object]]) -> list[list[dict[str, object]]]:
    segments: list[list[dict[str, object]]] = []
    current: list[dict[str, object]] = []
    previous: dict[str, object] | None = None

    for row in rows:
        key = (row["stage"], row["target_channel"])
        if key not in METRIC_MAP:
            previous = row
            continue

        starts_new = previous is None
        if previous is not None:
            previous_key = (previous["stage"], previous["target_channel"])
            if key != previous_key:
                starts_new = True
            elif float(row["sweep_value"]) <= float(previous["sweep_value"]) - 1e-9:
                starts_new = True

        if starts_new:
            if current:
                segments.append(current)
            current = [row]
        else:
            current.append(row)
        previous = row

    if current:
        segments.append(current)

    return [segment for segment in segments if len(segment) >= 2]


def default_output_path(csv_path: Path, out_dir: Path | None = None) -> Path:
    target_dir = out_dir or DEFAULT_IMAGE_DIR
    return target_dir / f"{csv_path.stem}_all_stages.png"


def plot_flow_metrics(
    csv_path: str | Path,
    out_path: str | Path | None = None,
    out_dir: str | Path | None = None,
    title: str | None = None,
) -> Path:
    import matplotlib.pyplot as plt

    csv_file = Path(csv_path)
    output_dir = Path(out_dir) if out_dir is not None else None
    image_path = Path(out_path) if out_path is not None else default_output_path(csv_file, output_dir)
    image_path.parent.mkdir(parents=True, exist_ok=True)

    rows = load_metric_rows(csv_file)
    segments = split_scan_segments(rows)
    if not segments:
        raise ValueError(f"no plottable DPMZM scan segments found in {csv_file}")

    cols = 3
    panel_count = len(segments)
    plot_rows = math.ceil(panel_count / cols)
    fig, axes = plt.subplots(plot_rows, cols, figsize=(6.2 * cols, 3.7 * plot_rows), squeeze=False)
    fig.suptitle(title or f"DPMZM scan flow: {csv_file.stem}", fontsize=16)

    for index, segment in enumerate(segments):
        ax = axes[index // cols][index % cols]
        stage = str(segment[0]["stage"])
        target = str(segment[0]["target_channel"])
        metric_col, label = METRIC_MAP[(stage, target)]
        x_values = [float(row["sweep_value"]) for row in segment]
        y_values = [mag_to_dbm(float(row[metric_col])) for row in segment]

        ax.plot(x_values, y_values, "-o", color="black", markersize=2.5, linewidth=1.0)
        best_index = min(range(len(y_values)), key=lambda item: y_values[item])
        best_x = x_values[best_index]
        best_y = y_values[best_index]
        ax.scatter([best_x], [best_y], color="red", s=36, zorder=4)
        ax.annotate(
            f"{best_x:+.3f} V\n{best_y:.1f} dBm",
            xy=(best_x, best_y),
            xytext=(6, 8),
            textcoords="offset points",
            color="red",
            fontsize=8,
        )
        ax.set_title(f"{index + 1}. {label}, blocks={segment[0]['blocks']}", fontsize=10)
        ax.set_xlabel(f"{target.upper()} bias voltage (V)")
        ax.set_ylabel("Equivalent power (dBm, 50 Ohm)")
        ax.grid(True, alpha=0.3)

    for index in range(panel_count, plot_rows * cols):
        axes[index // cols][index % cols].axis("off")

    fig.tight_layout(rect=[0.0, 0.0, 1.0, 0.97])
    fig.savefig(image_path, dpi=180)
    plt.close(fig)
    return image_path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Plot all DPMZM positive-branch scan stages from a metrics CSV."
    )
    parser.add_argument("csv", type=Path, help="Metrics CSV generated by run_dpmzm_positive_branch_flow.py")
    parser.add_argument("--out", type=Path, default=None, help="Output PNG path.")
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_IMAGE_DIR, help="Output image directory.")
    parser.add_argument("--title", default=None, help="Optional figure title.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    image_path = plot_flow_metrics(args.csv, out_path=args.out, out_dir=args.out_dir, title=args.title)
    print(image_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
