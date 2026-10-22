#!/usr/bin/env python3
"""Render RV1126 per-stage latency CSV as a dependency-free SVG dashboard."""

import argparse
import csv
import html
import math
import sys
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple


STAGE_ORDER = (
    "ai.rga_age",
    "ai.queue",
    "ai.infer",
    "ai.total",
    "osd.wait",
    "osd.update",
    "main.venc_age",
    "main.rtsp_wait",
    "main.send",
    "main.server_total",
    "sub.venc_age",
    "sub.rtsp_wait",
    "sub.send",
    "sub.server_total",
)

PALETTE = (
    "#2563eb", "#0891b2", "#7c3aed", "#db2777", "#059669", "#65a30d",
    "#ea580c", "#d97706", "#dc2626", "#9333ea", "#0284c7", "#0d9488",
    "#ca8a04", "#e11d48",
)


def nearest_rank(values: Sequence[int], percentage: int) -> int:
    ordered = sorted(values)
    rank = math.ceil(len(ordered) * percentage / 100.0)
    return ordered[max(0, rank - 1)]


def read_samples(path: Path) -> Dict[str, List[Tuple[int, int]]]:
    required = {"observed_monotonic_us", "stage", "latency_us"}
    grouped: Dict[str, List[Tuple[int, int]]] = {}
    with path.open(newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source)
        if not reader.fieldnames or not required.issubset(reader.fieldnames):
            missing = ", ".join(sorted(required - set(reader.fieldnames or ())))
            raise ValueError(f"missing CSV columns: {missing}")
        for line_number, row in enumerate(reader, start=2):
            try:
                observed = int(row["observed_monotonic_us"])
                latency = int(row["latency_us"])
                stage = row["stage"].strip()
            except (TypeError, ValueError) as error:
                raise ValueError(f"invalid sample on line {line_number}: {error}") from error
            if not stage or observed < 0 or latency < 0:
                raise ValueError(f"invalid sample on line {line_number}")
            grouped.setdefault(stage, []).append((observed, latency))
    if not grouped:
        raise ValueError("CSV contains no latency samples")
    for samples in grouped.values():
        samples.sort()
    return grouped


def downsample_min_max(samples: Sequence[Tuple[int, int]], limit: int = 1200):
    if len(samples) <= limit:
        return list(samples)
    bucket_size = math.ceil(len(samples) / (limit // 2))
    result: List[Tuple[int, int]] = []
    for start in range(0, len(samples), bucket_size):
        bucket = samples[start:start + bucket_size]
        low = min(bucket, key=lambda item: item[1])
        high = max(bucket, key=lambda item: item[1])
        result.extend(sorted({low, high}))
    return result


def svg_text(x: float, y: float, value: str, css_class: str, anchor: str = "start") -> str:
    return (f'<text x="{x:.1f}" y="{y:.1f}" class="{css_class}" '
            f'text-anchor="{anchor}">{html.escape(value)}</text>')


def render_svg(grouped: Dict[str, List[Tuple[int, int]]], output: Path) -> None:
    stages = [stage for stage in STAGE_ORDER if stage in grouped]
    stages.extend(sorted(set(grouped) - set(stages)))
    time_min = min(samples[0][0] for samples in grouped.values())
    time_max = max(samples[-1][0] for samples in grouped.values())
    time_span = max(1, time_max - time_min)

    columns = 2
    page_width = 1500
    outer = 38
    gap = 26
    panel_width = (page_width - outer * 2 - gap) / columns
    panel_height = 210
    rows = math.ceil(len(stages) / columns)
    page_height = 105 + rows * panel_height + 38
    plot_left, plot_top, plot_right, plot_bottom = 72, 64, 20, 38

    svg = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        (f'<svg xmlns="http://www.w3.org/2000/svg" width="{page_width}" '
         f'height="{page_height}" viewBox="0 0 {page_width} {page_height}">'),
        "<style>",
        "text{font-family:DejaVu Sans,Arial,sans-serif;fill:#172033}",
        ".title{font-size:26px;font-weight:700}.subtitle{font-size:13px;fill:#526078}",
        ".stage{font-size:16px;font-weight:700}.stat{font-size:11px;fill:#526078}",
        ".axis{font-size:10px;fill:#68758b}.grid{stroke:#d9e0ea;stroke-width:1}",
        ".panel{fill:#ffffff;stroke:#d8e0eb;stroke-width:1}",
        ".p95{stroke:#f59e0b;stroke-width:1;stroke-dasharray:5 4}",
        ".p99{stroke:#ef4444;stroke-width:1;stroke-dasharray:3 3}",
        "</style>",
        f'<rect width="{page_width}" height="{page_height}" fill="#f4f7fb"/>',
        svg_text(outer, 42, "RV1126 runtime stage latency", "title"),
        svg_text(outer, 68,
                 (f"{sum(len(values) for values in grouped.values()):,} samples · "
                  f"{time_span / 1_000_000:.2f} s · unit: ms"), "subtitle"),
    ]

    for index, stage in enumerate(stages):
        column, row = index % columns, index // columns
        panel_x = outer + column * (panel_width + gap)
        panel_y = 88 + row * panel_height
        x0 = panel_x + plot_left
        y0 = panel_y + plot_top
        plot_width = panel_width - plot_left - plot_right
        plot_height = panel_height - plot_top - plot_bottom
        samples = grouped[stage]
        values = [latency for _, latency in samples]
        p50 = nearest_rank(values, 50)
        p95 = nearest_rank(values, 95)
        p99 = nearest_rank(values, 99)
        maximum = max(values)
        y_max = max(1, maximum)
        color = PALETTE[index % len(PALETTE)]

        svg.append(f'<rect class="panel" x="{panel_x:.1f}" y="{panel_y:.1f}" '
                   f'width="{panel_width:.1f}" height="{panel_height - 12:.1f}" rx="8"/>')
        svg.append(svg_text(panel_x + 16, panel_y + 25, stage, "stage"))
        stats = (f"n={len(values):,}   P50={p50 / 1000:.2f}   P95={p95 / 1000:.2f}   "
                 f"P99={p99 / 1000:.2f}   max={maximum / 1000:.2f}")
        svg.append(svg_text(panel_x + 16, panel_y + 45, stats, "stat"))

        for fraction in (0.0, 0.5, 1.0):
            y = y0 + plot_height * (1.0 - fraction)
            svg.append(f'<line class="grid" x1="{x0:.1f}" y1="{y:.1f}" '
                       f'x2="{x0 + plot_width:.1f}" y2="{y:.1f}"/>')
            svg.append(svg_text(x0 - 7, y + 3, f"{y_max * fraction / 1000:.1f}",
                                 "axis", "end"))

        def y_position(value: int) -> float:
            return y0 + plot_height * (1.0 - value / y_max)

        svg.append(f'<line class="p95" x1="{x0:.1f}" y1="{y_position(p95):.1f}" '
                   f'x2="{x0 + plot_width:.1f}" y2="{y_position(p95):.1f}"/>')
        svg.append(f'<line class="p99" x1="{x0:.1f}" y1="{y_position(p99):.1f}" '
                   f'x2="{x0 + plot_width:.1f}" y2="{y_position(p99):.1f}"/>')

        points = []
        for timestamp, latency in downsample_min_max(samples):
            x = x0 + (timestamp - time_min) / time_span * plot_width
            points.append(f"{x:.1f},{y_position(latency):.1f}")
        svg.append(f'<polyline fill="none" stroke="{color}" stroke-width="1.35" '
                   f'stroke-linejoin="round" points="{" ".join(points)}"/>')
        svg.append(svg_text(x0, y0 + plot_height + 18, "0 s", "axis"))
        svg.append(svg_text(x0 + plot_width, y0 + plot_height + 18,
                             f"{time_span / 1_000_000:.1f} s", "axis", "end"))

    svg.append("</svg>")
    output.write_text("\n".join(svg) + "\n", encoding="utf-8")


def print_summary(grouped: Dict[str, List[Tuple[int, int]]]) -> None:
    print("stage,count,p50_ms,p95_ms,p99_ms,max_ms")
    ordered = [stage for stage in STAGE_ORDER if stage in grouped]
    ordered.extend(sorted(set(grouped) - set(ordered)))
    for stage in ordered:
        values = [latency for _, latency in grouped[stage]]
        print(f"{stage},{len(values)},{nearest_rank(values, 50) / 1000:.3f},"
              f"{nearest_rank(values, 95) / 1000:.3f},"
              f"{nearest_rank(values, 99) / 1000:.3f},{max(values) / 1000:.3f}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path, help="CSV pulled from the RV1126 application")
    parser.add_argument("output", nargs="?", type=Path,
                        help="output SVG (default: input name with .svg suffix)")
    args = parser.parse_args()
    output = args.output or args.csv.with_suffix(".svg")
    try:
        grouped = read_samples(args.csv)
        render_svg(grouped, output)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print_summary(grouped)
    print(f"SVG: {output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
