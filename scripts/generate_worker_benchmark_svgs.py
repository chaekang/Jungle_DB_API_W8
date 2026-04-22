#!/usr/bin/env python3

from __future__ import annotations

import math
import re
from pathlib import Path
from typing import Dict, List, Tuple


ROOT_DIR = Path(__file__).resolve().parent.parent
SOURCE_MD = ROOT_DIR / "docs" / "worker_benchmark_comparison.md"
OUTPUT_DIR = ROOT_DIR / "docs" / "assets" / "worker-benchmark"
SUMMARY_MD = ROOT_DIR / "docs" / "worker_benchmark_visuals.md"

SVG_WIDTH = 1600
SVG_HEIGHT = 1200
PADDING = 48
HEADER_HEIGHT = 140
CARD_GAP = 28
COLORS = ["#1d4ed8", "#ea580c", "#059669", "#7c3aed", "#dc2626", "#0891b2"]

PERF_TABLE_TITLES = {
    "4.1 Read-heavy": "01_read_heavy_baseline",
    "4.2 Mixed": "02_mixed_baseline",
    "8.3 pilot 결과": "03_simulated_work_pilot",
    "8.4 본 실험 결과": "04_read_heavy_simulated_work_500us",
}

BETTER_DIRECTION = {
    "throughput (rps)": "higher",
    "avg queue wait (ms)": "lower",
    "avg response (ms)": "lower",
    "p95 success (ms)": "lower",
    "503": "lower",
    "avg execution (ms)": "lower",
    "worker busy ratio": "higher",
}


def clean_cell(text: str) -> str:
    return text.strip().strip("|").strip().replace("`", "")


def slugify(text: str) -> str:
    lowered = text.lower()
    lowered = re.sub(r"[^a-z0-9]+", "_", lowered)
    return lowered.strip("_")


def parse_md_tables(lines: List[str]) -> List[Dict[str, object]]:
    tables: List[Dict[str, object]] = []
    heading_stack: Dict[int, str] = {}
    paragraph_buffer: List[str] = []
    index = 0

    while index < len(lines):
        line = lines[index].rstrip("\n")

        if line.startswith("#"):
            hashes, _, title = line.partition(" ")
            heading_stack[len(hashes)] = title.strip()
            for level in list(heading_stack):
                if level > len(hashes):
                    del heading_stack[level]
            paragraph_buffer = []
            index += 1
            continue

        if line.startswith("|") and index + 1 < len(lines) and lines[index + 1].lstrip().startswith("| ---"):
            table_lines = [line.rstrip("\n")]
            index += 1
            while index < len(lines) and lines[index].startswith("|"):
                table_lines.append(lines[index].rstrip("\n"))
                index += 1

            headers = [clean_cell(cell) for cell in table_lines[0].split("|")[1:-1]]
            rows = []
            for row_line in table_lines[2:]:
                rows.append([clean_cell(cell) for cell in row_line.split("|")[1:-1]])

            context = " / ".join(heading_stack[level] for level in sorted(heading_stack))
            preceding_text = ""
            for paragraph_line in reversed(paragraph_buffer):
                if paragraph_line.strip():
                    preceding_text = paragraph_line.strip()
                    break

            tables.append({
                "context": context,
                "title": heading_stack.get(3) or heading_stack.get(4) or heading_stack.get(2) or "Table",
                "headers": headers,
                "rows": rows,
                "preceding_text": preceding_text,
            })
            paragraph_buffer = []
            continue

        if line.strip():
            paragraph_buffer.append(line)
        else:
            paragraph_buffer.append("")
        index += 1

    return tables


def looks_like_performance_table(table: Dict[str, object]) -> bool:
    headers = table["headers"]  # type: ignore[assignment]
    return isinstance(headers, list) and "throughput (rps)" in headers


def parse_numeric(value: str) -> float:
    match = re.search(r"-?\d+(?:\.\d+)?", value.replace(",", ""))
    if match is None:
        raise ValueError(f"Could not parse numeric value from {value!r}")
    return float(match.group(0))


def metric_note(metric: str) -> str:
    direction = BETTER_DIRECTION.get(metric)
    if direction == "higher":
        return "higher is better"
    if direction == "lower":
        return "lower is better"
    return ""


def choose_grid(metric_count: int) -> Tuple[int, int]:
    columns = 2 if metric_count > 2 else metric_count
    rows = math.ceil(metric_count / columns)
    return columns, rows


def rect(x: float, y: float, width: float, height: float, fill: str, stroke: str = "none", stroke_width: int = 1, rx: int = 0) -> str:
    return (
        f'<rect x="{x:.1f}" y="{y:.1f}" width="{width:.1f}" height="{height:.1f}" '
        f'fill="{fill}" stroke="{stroke}" stroke-width="{stroke_width}" rx="{rx}" />'
    )


def text(x: float, y: float, content: str, size: int = 24, fill: str = "#111827", weight: str = "400",
         anchor: str = "start") -> str:
    escaped = (
        content.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
    )
    return (
        f'<text x="{x:.1f}" y="{y:.1f}" font-family="Helvetica, Arial, sans-serif" '
        f'font-size="{size}" font-weight="{weight}" fill="{fill}" text-anchor="{anchor}">{escaped}</text>'
    )


def line(x1: float, y1: float, x2: float, y2: float, stroke: str = "#d1d5db", stroke_width: int = 1) -> str:
    return (
        f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" '
        f'stroke="{stroke}" stroke-width="{stroke_width}" />'
    )


def format_value(metric: str, value: float) -> str:
    if metric == "503":
        return f"{int(round(value))}"
    if value >= 100:
        return f"{value:.1f}"
    if value >= 10:
        return f"{value:.2f}"
    return f"{value:.3f}"


def render_metric_card(metric: str, labels: List[str], values: List[float], x: float, y: float, width: float, height: float) -> str:
    parts: List[str] = []
    parts.append(rect(x, y, width, height, "#ffffff", "#dbe2ea", 1, 18))
    parts.append(text(x + 24, y + 34, metric, 26, "#0f172a", "700"))
    note = metric_note(metric)
    if note:
        parts.append(text(x + 24, y + 64, note, 16, "#475569"))

    chart_left = x + 72
    chart_right = x + width - 24
    chart_top = y + 96
    chart_bottom = y + height - 72
    chart_height = chart_bottom - chart_top
    chart_width = chart_right - chart_left
    max_value = max(values) if values else 1.0
    scaled_max = max_value * 1.12 if max_value > 0 else 1.0

    parts.append(line(chart_left, chart_bottom, chart_right, chart_bottom, "#94a3b8", 2))

    ticks = 4
    for tick in range(ticks + 1):
        tick_value = scaled_max * (tick / ticks)
        tick_y = chart_bottom - chart_height * (tick / ticks)
        parts.append(line(chart_left, tick_y, chart_right, tick_y, "#e5e7eb", 1))
        parts.append(text(chart_left - 12, tick_y + 5, format_value(metric, tick_value), 14, "#64748b", "400", "end"))

    slot_width = chart_width / max(len(values), 1)
    bar_width = min(56, slot_width * 0.55)

    for index, (label_value, metric_value) in enumerate(zip(labels, values)):
        center_x = chart_left + slot_width * index + slot_width / 2.0
        bar_height = 0 if scaled_max == 0 else chart_height * (metric_value / scaled_max)
        bar_x = center_x - bar_width / 2.0
        bar_y = chart_bottom - bar_height
        color = COLORS[index % len(COLORS)]

        parts.append(rect(bar_x, bar_y, bar_width, bar_height, color, "none", 0, 10))
        parts.append(text(center_x, bar_y - 10, format_value(metric, metric_value), 14, "#1f2937", "700", "middle"))
        parts.append(text(center_x, chart_bottom + 28, label_value, 16, "#334155", "600", "middle"))

    return "\n".join(parts)


def render_dashboard(table: Dict[str, object], output_path: Path) -> None:
    headers = table["headers"]  # type: ignore[assignment]
    rows = table["rows"]  # type: ignore[assignment]
    title = table["title"]  # type: ignore[assignment]
    preceding_text = table["preceding_text"]  # type: ignore[assignment]

    label_header = headers[0]
    metric_headers = headers[1:]
    labels = [row[0] for row in rows]
    metric_values: Dict[str, List[float]] = {
        metric: [parse_numeric(row[index + 1]) for row in rows]
        for index, metric in enumerate(metric_headers)
    }

    columns, rows_count = choose_grid(len(metric_headers))
    usable_width = SVG_WIDTH - PADDING * 2
    usable_height = SVG_HEIGHT - PADDING * 2 - HEADER_HEIGHT
    card_width = (usable_width - CARD_GAP * (columns - 1)) / columns
    card_height = (usable_height - CARD_GAP * (rows_count - 1)) / rows_count

    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{SVG_WIDTH}" height="{SVG_HEIGHT}" viewBox="0 0 {SVG_WIDTH} {SVG_HEIGHT}">',
        rect(0, 0, SVG_WIDTH, SVG_HEIGHT, "#f8fafc"),
        rect(PADDING, PADDING, SVG_WIDTH - PADDING * 2, SVG_HEIGHT - PADDING * 2, "#eef4ff", "none", 0, 30),
        text(PADDING + 24, PADDING + 48, f"Worker Benchmark Visuals: {title}", 40, "#0f172a", "700"),
        text(PADDING + 24, PADDING + 82, preceding_text or "Benchmark metrics dashboard", 20, "#334155", "400"),
        text(PADDING + 24, PADDING + 114, f"Category axis: {label_header}", 18, "#475569", "400"),
    ]

    for metric_index, metric in enumerate(metric_headers):
        row_index = metric_index // columns
        col_index = metric_index % columns
        card_x = PADDING + col_index * (card_width + CARD_GAP)
        card_y = PADDING + HEADER_HEIGHT + row_index * (card_height + CARD_GAP)
        parts.append(render_metric_card(
            metric,
            labels,
            metric_values[metric],
            card_x,
            card_y,
            card_width,
            card_height,
        ))

    legend_y = SVG_HEIGHT - PADDING - 14
    legend_x = PADDING + 24
    parts.append(text(legend_x, legend_y - 22, "Series", 18, "#334155", "700"))
    for index, label_value in enumerate(labels):
        color = COLORS[index % len(COLORS)]
        parts.append(rect(legend_x + index * 150, legend_y - 12, 18, 18, color, "none", 0, 4))
        parts.append(text(legend_x + index * 150 + 26, legend_y + 2, label_value, 16, "#334155", "600"))

    parts.append("</svg>")
    output_path.write_text("\n".join(parts), encoding="utf-8")


def build_summary_md(outputs: List[Tuple[str, str, str]]) -> None:
    lines = [
        "# Worker Benchmark Visual Assets",
        "",
        "발표용으로 바로 사용할 수 있도록 Markdown 표를 SVG 그래프로 변환한 결과다.",
        "",
    ]

    for title, note, rel_path in outputs:
        lines.extend([
            f"## {title}",
            "",
            note,
            "",
            f"![{title}]({rel_path})",
            "",
        ])

    SUMMARY_MD.write_text("\n".join(lines), encoding="utf-8")


def main() -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    lines = SOURCE_MD.read_text(encoding="utf-8").splitlines()
    tables = [table for table in parse_md_tables(lines) if looks_like_performance_table(table)]

    outputs: List[Tuple[str, str, str]] = []
    for table in tables:
        title = str(table["title"])
        file_stem = PERF_TABLE_TITLES.get(title, slugify(title))
        output_path = OUTPUT_DIR / f"{file_stem}.svg"
        render_dashboard(table, output_path)
        note = str(table["preceding_text"] or "Performance table visualized as a multi-metric SVG dashboard.")
        rel_path = f"assets/worker-benchmark/{output_path.name}"
        outputs.append((title, note, rel_path))

    build_summary_md(outputs)


if __name__ == "__main__":
    main()
