#!/usr/bin/env python3

from __future__ import annotations

from pathlib import Path
from typing import Dict, List, Tuple


ROOT_DIR = Path(__file__).resolve().parent.parent
OUTPUT_DIR = ROOT_DIR / "docs" / "assets" / "worker-benchmark-highlights"
SUMMARY_MD = ROOT_DIR / "docs" / "worker_benchmark_presentation.md"

WIDTH = 1600
HEIGHT = 900
BG = "#f8fafc"
CARD = "#ffffff"
TEXT = "#0f172a"
MUTED = "#475569"
GRID = "#dbe2ea"
BLUE = "#2563eb"
ORANGE = "#ea580c"
GREEN = "#059669"
RED = "#dc2626"
PURPLE = "#7c3aed"


READ_HEAVY = {
    "workers": [1, 2, 4, 8],
    "throughput": [2870.684, 2894.732, 2673.925, 2852.914],
    "avg_response": [0.106, 0.047, 0.048, 0.047],
}

MIXED = {
    "workers": [1, 2, 4, 8],
    "throughput": [2814.102, 2853.471, 2880.291, 2763.494],
    "p95": [1.998, 1.431, 0.809, 3.588],
    "errors_503": [2705, 1682, 1177, 3082],
}

HEAVY_500 = {
    "workers": [1, 2, 4, 8],
    "throughput": [1816.162, 2865.493, 2857.416, 2854.494],
    "p95": [18.048, 1.861, 1.014, 0.989],
    "errors_503": [10969, 83, 107, 117],
}


def rect(x: float, y: float, w: float, h: float, fill: str, stroke: str = "none",
         stroke_width: int = 1, rx: int = 16) -> str:
    return (
        f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" '
        f'fill="{fill}" stroke="{stroke}" stroke-width="{stroke_width}" rx="{rx}" />'
    )


def line(x1: float, y1: float, x2: float, y2: float, stroke: str = GRID,
         stroke_width: int = 1) -> str:
    return (
        f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" '
        f'stroke="{stroke}" stroke-width="{stroke_width}" />'
    )


def text(x: float, y: float, content: str, size: int = 24, fill: str = TEXT,
         weight: str = "400", anchor: str = "start") -> str:
    escaped = (
        content.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
    )
    return (
        f'<text x="{x:.1f}" y="{y:.1f}" font-family="Helvetica, Arial, sans-serif" '
        f'font-size="{size}" font-weight="{weight}" fill="{fill}" text-anchor="{anchor}">{escaped}</text>'
    )


def format_value(metric: str, value: float) -> str:
    if metric == "503":
        return str(int(round(value)))
    if value >= 100:
        return f"{value:.0f}"
    if value >= 10:
        return f"{value:.2f}"
    return f"{value:.3f}"


def bar_chart(
    x: float,
    y: float,
    w: float,
    h: float,
    title: str,
    unit_hint: str,
    labels: List[str],
    values: List[float],
    color: str,
    emphasize_index: int | None = None,
) -> str:
    parts: List[str] = []
    parts.append(rect(x, y, w, h, CARD, "#dbe2ea", 1, 18))
    parts.append(text(x + 24, y + 38, title, 28, TEXT, "700"))
    parts.append(text(x + 24, y + 66, unit_hint, 16, MUTED))

    left = x + 72
    right = x + w - 24
    top = y + 98
    bottom = y + h - 78
    chart_h = bottom - top
    chart_w = right - left
    max_value = max(values) if values else 1.0
    scaled_max = max_value * 1.15 if max_value > 0 else 1.0

    for tick in range(5):
        tick_ratio = tick / 4.0
        tick_y = bottom - chart_h * tick_ratio
        tick_value = scaled_max * tick_ratio
        parts.append(line(left, tick_y, right, tick_y))
        parts.append(text(left - 10, tick_y + 5, format_value(title, tick_value), 14, MUTED, "400", "end"))

    parts.append(line(left, bottom, right, bottom, "#94a3b8", 2))

    slot = chart_w / max(len(values), 1)
    bar_w = min(70, slot * 0.58)
    for idx, (label, value) in enumerate(zip(labels, values)):
        cx = left + slot * idx + slot / 2.0
        bh = chart_h * (value / scaled_max) if scaled_max else 0.0
        bx = cx - bar_w / 2.0
        by = bottom - bh
        fill = color if emphasize_index != idx else RED
        parts.append(rect(bx, by, bar_w, bh, fill, "none", 0, 10))
        parts.append(text(cx, by - 10, format_value(title, value), 15, TEXT, "700", "middle"))
        parts.append(text(cx, bottom + 28, label, 16, MUTED, "600", "middle"))

    return "\n".join(parts)


def callout_box(x: float, y: float, w: float, h: float, title_text: str, body_lines: List[str],
               accent: str) -> str:
    parts = [
        rect(x, y, w, h, "#fffaf5", accent, 2, 20),
        text(x + 24, y + 40, title_text, 28, TEXT, "700"),
    ]
    cursor_y = y + 82
    for line_text in body_lines:
        parts.append(text(x + 24, cursor_y, line_text, 20, MUTED, "400"))
        cursor_y += 32
    return "\n".join(parts)


def svg_shell(title_text: str, subtitle: str, inner: List[str]) -> str:
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" height="{HEIGHT}" viewBox="0 0 {WIDTH} {HEIGHT}">',
        rect(0, 0, WIDTH, HEIGHT, BG, "none", 0, 0),
        text(56, 70, title_text, 42, TEXT, "700"),
        text(56, 108, subtitle, 22, MUTED, "400"),
        *inner,
        "</svg>",
    ]
    return "\n".join(parts)


def build_read_heavy_baseline() -> Tuple[str, str]:
    workers = [str(worker) for worker in READ_HEAVY["workers"]]
    throughput_delta = ((max(READ_HEAVY["throughput"]) / READ_HEAVY["throughput"][0]) - 1.0) * 100.0
    inner = [
        bar_chart(56, 150, 720, 520, "throughput (rps)", "higher is better", workers, READ_HEAVY["throughput"], BLUE, 1),
        bar_chart(824, 150, 720, 520, "avg response (ms)", "lower is better", workers, READ_HEAVY["avg_response"], ORANGE, 0),
        callout_box(
            56,
            710,
            1488,
            138,
            "Presentation Point",
            [
                f"read-heavy baseline does not meaningfully scale with more workers. Peak throughput gain is only +{throughput_delta:.1f}%.",
                "The main effect of multi-worker here is modest latency stabilization, not a real throughput jump.",
            ],
            BLUE,
        ),
    ]
    return (
        "Read-heavy Baseline: Throughput Barely Changes",
        svg_shell("Read-heavy Baseline", "Single worker is already enough for the current id-index read path.", inner),
    )


def build_mixed_sweet_spot() -> Tuple[str, str]:
    workers = [str(worker) for worker in MIXED["workers"]]
    inner = [
        bar_chart(56, 150, 480, 520, "throughput (rps)", "higher is better", workers, MIXED["throughput"], BLUE, 2),
        bar_chart(560, 150, 480, 520, "p95 success (ms)", "lower is better", workers, MIXED["p95"], PURPLE, 3),
        bar_chart(1064, 150, 480, 520, "503", "lower is better", workers, MIXED["errors_503"], ORANGE, 3),
        callout_box(
            56,
            710,
            1488,
            138,
            "Presentation Point",
            [
                "Mixed workload has a sweet spot at 4 workers: best throughput, best p95, and the fewest 503s.",
                "Going from 4 to 8 workers makes the server worse again, which suggests contention and scheduling overhead.",
            ],
            GREEN,
        ),
    ]
    return (
        "Mixed Workload: 4 Workers Is the Sweet Spot",
        svg_shell("Mixed Workload", "This is the slide that supports choosing 4 workers instead of just increasing the count.", inner),
    )


def build_heavy_scaling() -> Tuple[str, str]:
    workers = [str(worker) for worker in HEAVY_500["workers"]]
    inner = [
        bar_chart(56, 150, 480, 520, "throughput (rps)", "higher is better", workers, HEAVY_500["throughput"], BLUE, 0),
        bar_chart(560, 150, 480, 520, "p95 success (ms)", "lower is better", workers, HEAVY_500["p95"], PURPLE, 0),
        bar_chart(1064, 150, 480, 520, "503", "lower is better", workers, HEAVY_500["errors_503"], ORANGE, 0),
        callout_box(
            56,
            710,
            1488,
            138,
            "Presentation Point",
            [
                "Once each request includes about 500us of CPU work, worker scaling becomes obvious.",
                "The original read-heavy path looked flat not because multi-worker is useless, but because the baseline work was too light.",
            ],
            RED,
        ),
    ]
    return (
        "Heavier Workload: Multi-worker Benefit Becomes Obvious",
        svg_shell("Simulated 500us CPU Work", "This slide explains why the original read-heavy test did not show a scaling win.", inner),
    )


def write_summary(entries: List[Tuple[str, str, str]]) -> None:
    lines = [
        "# Worker Benchmark Presentation Assets",
        "",
        "발표에서는 아래 3장만 쓰는 구성이 가장 자연스럽다.",
        "",
    ]

    for title_text, takeaway, rel_path in entries:
        lines.extend([
            f"## {title_text}",
            "",
            takeaway,
            "",
            f"![{title_text}]({rel_path})",
            "",
        ])

    SUMMARY_MD.write_text("\n".join(lines), encoding="utf-8")


def main() -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    charts = [
        ("01_read_heavy_key_point.svg", *build_read_heavy_baseline()),
        ("02_mixed_sweet_spot.svg", *build_mixed_sweet_spot()),
        ("03_heavy_work_scaling.svg", *build_heavy_scaling()),
    ]

    summary_entries: List[Tuple[str, str, str]] = []
    for filename, title_text, svg_content in charts:
        path = OUTPUT_DIR / filename
        path.write_text(svg_content, encoding="utf-8")
        if "Read-heavy Baseline" in title_text:
            takeaway = "포인트: 현재 read-heavy baseline은 이미 너무 가벼워서 worker 수를 늘려도 처리량이 거의 안 오른다."
        elif "Mixed Workload" in title_text:
            takeaway = "포인트: mixed workload에서는 4 workers가 최적점이다. 8로 늘리면 오히려 성능이 악화된다."
        else:
            takeaway = "포인트: 요청당 일을 더 무겁게 만들면 multi-worker의 가치가 확실히 드러난다."
        summary_entries.append((title_text, takeaway, f"assets/worker-benchmark-highlights/{filename}"))

    write_summary(summary_entries)


if __name__ == "__main__":
    main()
