#!/usr/bin/env python3
"""Plot OSA spectra and annotate carrier/first-order sideband suppression.

The script is intended for folders like:

  result/T41_07039/raw/*.txt
  result/T45_08869/raw/*.txt

It writes annotated PNG files to each sibling ``spectrum`` folder and a
summary CSV under the result root.
"""

from __future__ import annotations

import argparse
import csv
import math
import re
from dataclasses import dataclass
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.ticker import ScalarFormatter


DEFAULT_RESULT_ROOT = Path(r"C:\Users\Administrator\Desktop\DPMZM_contral_bais\result")
DEFAULT_TARGETS = ("T41", "T45")


@dataclass
class SpectrumTrace:
    path: Path
    wavelength_nm: list[float]
    power_dbm: list[float]

    @property
    def name(self) -> str:
        name = self.path.stem
        while name.endswith("_Spectrum"):
            name = name[: -len("_Spectrum")]
        return name


@dataclass
class SpectrumMarks:
    carrier_idx: int
    suppressed_idx: int
    unsuppressed_idx: int

    @property
    def indices(self) -> tuple[int, int, int]:
        return (self.suppressed_idx, self.carrier_idx, self.unsuppressed_idx)


def _try_float(text: str) -> float | None:
    try:
        value = float(text)
    except ValueError:
        return None
    return value if math.isfinite(value) else None


def load_spectrum(path: Path) -> SpectrumTrace:
    x: list[float] = []
    y: list[float] = []
    for raw_line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        parts = [p for p in re.split(r"[\s,;]+", raw_line.strip()) if p]
        if len(parts) < 2:
            continue
        wavelength = _try_float(parts[0])
        power = _try_float(parts[1])
        if wavelength is None or power is None:
            continue
        if 1200.0 <= wavelength <= 1700.0 and -200.0 <= power <= 100.0:
            x.append(wavelength)
            y.append(power)
    if len(x) < 10:
        raise ValueError(f"not enough spectrum points: {path}")
    points = sorted(zip(x, y), key=lambda item: item[0])
    return SpectrumTrace(
        path=path,
        wavelength_nm=[p[0] for p in points],
        power_dbm=[p[1] for p in points],
    )


def format_axis(ax) -> None:
    formatter = ScalarFormatter(useOffset=False)
    formatter.set_scientific(False)
    ax.xaxis.set_major_formatter(formatter)


def safe_name(path: Path, suffix: str = "_sideband_annotated") -> str:
    name = path.stem
    while name.endswith("_Spectrum"):
        name = name[: -len("_Spectrum")]
    name = re.sub(r"[^\w.\-]+", "_", name, flags=re.UNICODE).strip("_")
    return f"{name}{suffix}.png"


def moving_average(values: list[float], window: int) -> list[float]:
    if window <= 1:
        return values[:]
    if window % 2 == 0:
        window += 1
    half = window // 2
    out: list[float] = []
    for i in range(len(values)):
        lo = max(0, i - half)
        hi = min(len(values), i + half + 1)
        out.append(sum(values[lo:hi]) / (hi - lo))
    return out


def candidate_peak_indices(x: list[float], y: list[float]) -> list[int]:
    """Return likely spectral-line peak indices."""
    if len(x) < 3:
        return list(range(len(x)))

    sorted_y = sorted(y)
    median = sorted_y[len(sorted_y) // 2]
    p95 = sorted_y[int(0.95 * (len(sorted_y) - 1))]
    threshold = max(median + 5.0, p95 + 0.5)

    candidates: set[int] = set()
    group: list[int] = []
    for idx, power in enumerate(y):
        if power >= threshold:
            group.append(idx)
        elif group:
            candidates.add(max(group, key=lambda i: y[i]))
            group = []
    if group:
        candidates.add(max(group, key=lambda i: y[i]))

    smoothed = moving_average(y, 5)
    for idx in range(1, len(y) - 1):
        if smoothed[idx] >= smoothed[idx - 1] and smoothed[idx] >= smoothed[idx + 1]:
            lo = max(0, idx - 2)
            hi = min(len(y), idx + 3)
            candidates.add(max(range(lo, hi), key=lambda i: y[i]))

    candidates.add(max(range(len(y)), key=lambda i: y[i]))
    separated: list[int] = []
    for idx in sorted(candidates, key=lambda i: y[i], reverse=True):
        if all(abs(x[idx] - x[old]) >= 0.006 for old in separated):
            separated.append(idx)
        if len(separated) >= 30:
            break
    return sorted(separated, key=lambda i: x[i])


def identify_carrier_and_sidebands(trace: SpectrumTrace) -> SpectrumMarks:
    """Find the sideband-carrier-sideband triplet.

    In the current SSB-like spectra, the carrier is the middle peak in
    wavelength, not necessarily the strongest peak.
    """
    x = trace.wavelength_nm
    y = trace.power_dbm
    candidates = candidate_peak_indices(x, y)
    best: tuple[float, tuple[int, int, int]] | None = None

    for ai in range(len(candidates)):
        for bi in range(ai + 1, len(candidates)):
            for ci in range(bi + 1, len(candidates)):
                left = candidates[ai]
                center = candidates[bi]
                right = candidates[ci]
                dl = x[center] - x[left]
                dr = x[right] - x[center]
                if dl <= 0.0 or dr <= 0.0:
                    continue
                if not (0.015 <= dl <= 0.12 and 0.015 <= dr <= 0.12):
                    continue
                symmetry = abs(dl - dr) / max(dl, dr)
                if symmetry > 0.45:
                    continue
                score = y[left] + y[center] + y[right] - 35.0 * symmetry
                if best is None or score > best[0]:
                    best = (score, (left, center, right))

    if best is None:
        chosen = sorted(candidates, key=lambda i: y[i], reverse=True)[:3]
        if len(chosen) < 3:
            chosen = sorted(range(len(y)), key=lambda i: y[i], reverse=True)[:3]
        left, center, right = sorted(chosen, key=lambda i: x[i])
    else:
        left, center, right = best[1]

    if y[left] >= y[right]:
        unsuppressed = left
        suppressed = right
    else:
        unsuppressed = right
        suppressed = left
    return SpectrumMarks(
        carrier_idx=center,
        suppressed_idx=suppressed,
        unsuppressed_idx=unsuppressed,
    )


def plot_trace(trace: SpectrumTrace, out_dir: Path) -> tuple[Path, dict[str, float | str]]:
    x = trace.wavelength_nm
    y = trace.power_dbm
    marks = identify_carrier_and_sidebands(trace)
    carrier = marks.carrier_idx
    unsup = marks.unsuppressed_idx
    supp = marks.suppressed_idx

    carrier_gap_db = y[unsup] - y[carrier]
    sideband_gap_db = y[unsup] - y[supp]

    fig, ax = plt.subplots(figsize=(13.5, 6.6), constrained_layout=True)
    ax.plot(x, y, color="#008a2e", linewidth=1.35, label="OSA trace")

    marker_specs = [
        (unsup, "Unsuppressed 1st sideband", "#d62728"),
        (carrier, "Carrier", "#ff8c00"),
        (supp, "Suppressed 1st sideband", "#1f77b4"),
    ]
    y_top = max(y)
    y_span = max(y) - min(y)
    for idx, label, color in marker_specs:
        if y_span > 0.0 and y[idx] > y_top - 0.18 * y_span:
            text_offset = (10, -52)
        elif idx == carrier:
            text_offset = (10, -44)
        else:
            text_offset = (10, 14)
        ax.scatter([x[idx]], [y[idx]], s=48, color=color, zorder=4)
        ax.annotate(
            f"{label}\n{x[idx]:.4f} nm\n{y[idx]:.2f} dBm",
            xy=(x[idx], y[idx]),
            xytext=text_offset,
            textcoords="offset points",
            color=color,
            fontsize=9,
            arrowprops={"arrowstyle": "->", "color": color, "lw": 0.9},
        )

    summary = (
        f"Unsuppressed - Carrier = {carrier_gap_db:.2f} dB\n"
        f"Unsuppressed - Suppressed = {sideband_gap_db:.2f} dB"
    )
    ax.text(
        0.02,
        0.96,
        summary,
        transform=ax.transAxes,
        va="top",
        ha="left",
        fontsize=11,
        bbox={"boxstyle": "round,pad=0.4", "facecolor": "white", "edgecolor": "#008a2e", "alpha": 0.88},
    )

    ax.set_title(trace.name)
    ax.set_xlabel("Wavelength (nm)")
    ax.set_ylabel("Optical power (dBm)")
    ax.grid(True, alpha=0.28)
    ax.set_xlim(min(x), max(x))
    y_margin = max(1.0, (max(y) - min(y)) * 0.08)
    ax.set_ylim(min(y) - y_margin, max(y) + y_margin)
    format_axis(ax)
    ax.legend(loc="lower right", fontsize=9)

    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / safe_name(trace.path)
    fig.savefig(out_path, dpi=180)
    plt.close(fig)

    row: dict[str, float | str] = {
        "group": trace.path.parents[1].name,
        "file": trace.path.name,
        "carrier_nm": x[carrier],
        "carrier_dbm": y[carrier],
        "unsuppressed_sideband_nm": x[unsup],
        "unsuppressed_sideband_dbm": y[unsup],
        "suppressed_sideband_nm": x[supp],
        "suppressed_sideband_dbm": y[supp],
        "unsuppressed_minus_carrier_db": carrier_gap_db,
        "unsuppressed_minus_suppressed_db": sideband_gap_db,
        "plot": str(out_path),
    }
    return out_path, row


def find_target_dirs(root: Path, targets: tuple[str, ...]) -> list[Path]:
    dirs: list[Path] = []
    for target in targets:
        matches = sorted(p for p in root.iterdir() if p.is_dir() and p.name.startswith(target))
        dirs.extend(matches)
    return dirs


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Plot spectra with carrier/sideband annotations.")
    parser.add_argument("--root", type=Path, default=DEFAULT_RESULT_ROOT, help="result root directory")
    parser.add_argument("--targets", nargs="*", default=list(DEFAULT_TARGETS), help="target folder prefixes")
    parser.add_argument("--pattern", default="*.txt", help="raw spectrum file glob pattern")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = args.root.resolve()
    target_dirs = find_target_dirs(root, tuple(args.targets))
    if not target_dirs:
        raise SystemExit(f"No target directories found under {root}: {args.targets}")

    rows: list[dict[str, float | str]] = []
    for target_dir in target_dirs:
        raw_dir = target_dir / "raw"
        out_dir = target_dir / "spectrum"
        files = sorted(raw_dir.glob(args.pattern))
        if not files:
            print(f"[skip] no raw files in {raw_dir}")
            continue
        print(f"[group] {target_dir.name}: {len(files)} raw file(s)")
        for file_path in files:
            trace = load_spectrum(file_path)
            out_path, row = plot_trace(trace, out_dir)
            rows.append(row)
            print(
                f"[plot] {file_path.name} -> {out_path.name}; "
                f"carrier gap={row['unsuppressed_minus_carrier_db']:.2f} dB, "
                f"sideband gap={row['unsuppressed_minus_suppressed_db']:.2f} dB"
            )

    if rows:
        target_tag = "_".join(args.targets) if args.targets else "all"
        target_tag = re.sub(r"[^\w.\-]+", "_", target_tag, flags=re.UNICODE).strip("_")
        summary_path = root / f"{target_tag}_sideband_summary.csv"
        with summary_path.open("w", newline="", encoding="utf-8-sig") as f:
            writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            writer.writeheader()
            writer.writerows(rows)
        print(f"[summary] {summary_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
