#!/usr/bin/env python3
import html
import math
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE_MD = ROOT / "docs" / "queue_size_experiment_results_20260422.md"
OUTPUT_DIR = ROOT / "docs" / "assets" / "queue_size_experiment_charts"
INDEX_MD = ROOT / "docs" / "queue_size_experiment_visuals.md"


def slugify(text):
    slug = re.sub(r"[^a-zA-Z0-9]+", "_", text).strip("_").lower()
    return slug or "chart"


def parse_time_to_ms(value):
    text = value.strip()
    if text.endswith("us"):
        return float(text[:-2]) / 1000.0
    if text.endswith("ms"):
        return float(text[:-2])
    return float(text)


def parse_number(value):
    text = value.strip().replace(",", "")
    if text.endswith("us") or text.endswith("ms"):
        return parse_time_to_ms(text)
    return float(text)


def md_table_cells(line):
    raw = line.strip().strip("|")
    return [cell.strip() for cell in raw.split("|")]


def extract_tables(markdown_text):
    lines = markdown_text.splitlines()
    h2 = ""
    h3 = ""
    tables = []
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith("## "):
            h2 = line[3:].strip()
        elif line.startswith("### "):
            h3 = line[4:].strip()
        elif line.startswith("|"):
            block = []
            while i < len(lines) and lines[i].startswith("|"):
                block.append(lines[i])
                i += 1
            if len(block) >= 3:
                header = md_table_cells(block[0])
                if header and header[0] == "queue":
                    rows = []
                    for row_line in block[2:]:
                        cells = md_table_cells(row_line)
                        if len(cells) == len(header):
                            rows.append(dict(zip(header, cells)))
                    tables.append({
                        "section": h2,
                        "title": h3,
                        "headers": header,
                        "rows": rows,
                    })
            continue
        i += 1
    return tables


def svg_text(x, y, text, size=16, weight="400", fill="#111827", anchor="start"):
    escaped = html.escape(str(text))
    return (
        f'<text x="{x}" y="{y}" font-size="{size}" font-weight="{weight}" '
        f'fill="{fill}" text-anchor="{anchor}" '
        f'font-family="Pretendard, Apple SD Gothic Neo, Noto Sans CJK KR, sans-serif">{escaped}</text>'
    )


def svg_rect(x, y, width, height, fill, rx=12, stroke="none", stroke_width=0):
    return (
        f'<rect x="{x}" y="{y}" width="{width}" height="{height}" rx="{rx}" '
        f'fill="{fill}" stroke="{stroke}" stroke-width="{stroke_width}" />'
    )


def format_metric_value(key, value):
    if key in {"completed", "median completed", "failed", "median failed", "503", "median 503"}:
        return f"{int(round(value)):,}"
    return f"{value:.3f} ms"


def build_palette(queues):
    base = {
        8: "#9CA3AF",
        32: "#2563EB",
        48: "#EA580C",
        64: "#059669",
        128: "#7C3AED",
    }
    fallback = ["#2563EB", "#EA580C", "#059669", "#DC2626", "#7C3AED", "#0F766E"]
    colors = {}
    for index, queue in enumerate(queues):
        colors[queue] = base.get(queue, fallback[index % len(fallback)])
    return colors


def nice_upper_bound(values):
    max_value = max(values) if values else 1.0
    if max_value <= 0:
        return 1.0
    exponent = math.floor(math.log10(max_value))
    fraction = max_value / (10 ** exponent)
    if fraction <= 1:
        nice_fraction = 1
    elif fraction <= 2:
        nice_fraction = 2
    elif fraction <= 5:
        nice_fraction = 5
    else:
        nice_fraction = 10
    return nice_fraction * (10 ** exponent)


def draw_metric_card(x, y, width, height, metric_name, metric_key, rows, colors):
    parts = [svg_rect(x, y, width, height, "#FFFFFF", rx=18, stroke="#E5E7EB", stroke_width=1)]
    parts.append(svg_text(x + 18, y + 28, metric_name, size=17, weight="600"))

    chart_x = x + 16
    chart_y = y + 48
    chart_w = width - 32
    chart_h = height - 82

    numeric_values = [parse_number(row[metric_key]) for row in rows]
    upper = nice_upper_bound(numeric_values)
    queues = [int(row["queue"]) for row in rows]
    bar_gap = 16
    count = max(len(rows), 1)
    bar_width = (chart_w - bar_gap * (count + 1)) / count

    parts.append(f'<line x1="{chart_x}" y1="{chart_y + chart_h}" x2="{chart_x + chart_w}" y2="{chart_y + chart_h}" stroke="#CBD5E1" stroke-width="1" />')
    parts.append(svg_text(chart_x, chart_y + chart_h + 20, "0", size=11, fill="#6B7280"))
    parts.append(svg_text(chart_x + chart_w, chart_y + 12, format_metric_value(metric_key, upper), size=11, fill="#6B7280", anchor="end"))

    for idx, row in enumerate(rows):
        queue = int(row["queue"])
        value = parse_number(row[metric_key])
        ratio = 0 if upper == 0 else value / upper
        bar_height = max(chart_h * ratio, 2)
        bar_x = chart_x + bar_gap + idx * (bar_width + bar_gap)
        bar_y = chart_y + chart_h - bar_height

        parts.append(svg_rect(bar_x, bar_y, bar_width, bar_height, colors[queue], rx=10))
        parts.append(svg_text(bar_x + bar_width / 2, chart_y + chart_h + 18, f"q{queue}", size=12, weight="600", fill="#334155", anchor="middle"))
        parts.append(svg_text(bar_x + bar_width / 2, bar_y - 8, format_metric_value(metric_key, value), size=12, weight="600", fill="#0F172A", anchor="middle"))

    return "\n".join(parts)


def build_svg(table, index):
    metrics = [
        ("completed", "completed" if "completed" in table["headers"] else "median completed"),
        ("failed", "failed" if "failed" in table["headers"] else "median failed"),
        ("503", "503" if "503" in table["headers"] else "median 503"),
        ("avg queue wait", "avg queue wait(ms)" if "avg queue wait(ms)" in table["headers"] else "median avg queue wait(ms)"),
        ("avg total response", "avg total response(ms)" if "avg total response(ms)" in table["headers"] else "median avg total response(ms)"),
        ("k6 p95", "k6 p95" if "k6 p95" in table["headers"] else "median k6 p95"),
    ]

    queues = [int(row["queue"]) for row in table["rows"]]
    colors = build_palette(queues)

    width = 1680
    height = 1160
    margin_x = 60
    top = 110
    card_w = 500
    card_h = 430
    gap_x = 30
    gap_y = 28

    title = table["title"]
    subtitle = f"{table['section']} / table {index}"
    legend_x = width - 60 - len(queues) * 120

    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#F8FAFC" />',
        svg_text(margin_x, 56, "Queue Size Experiment Charts", size=34, weight="700", fill="#0F172A"),
        svg_text(margin_x, 88, title, size=22, weight="600", fill="#1D4ED8"),
        svg_text(margin_x, 112, subtitle, size=14, fill="#475569"),
    ]

    cursor_x = legend_x
    for queue in queues:
        parts.append(svg_rect(cursor_x, 48, 20, 20, colors[queue], rx=6))
        parts.append(svg_text(cursor_x + 30, 63, f"q{queue}", size=14, weight="600", fill="#334155"))
        cursor_x += 110

    for idx, (label, key) in enumerate(metrics):
        row = idx // 3
        col = idx % 3
        x = margin_x + col * (card_w + gap_x)
        y = top + row * (card_h + gap_y)
        parts.append(draw_metric_card(x, y, card_w, card_h, label, key, table["rows"], colors))

    parts.append(svg_text(margin_x, height - 28, f"Source: {SOURCE_MD.relative_to(ROOT)}", size=13, fill="#64748B"))
    parts.append("</svg>")
    return "\n".join(parts)


def build_index(tables, chart_paths):
    lines = [
        "# Queue Size Experiment Visuals",
        "",
        "발표용으로 바로 사용할 수 있도록 결과 표를 `SVG` 차트로 변환한 파일들이다.",
        "",
        f"- 원본 문서: `{SOURCE_MD.relative_to(ROOT)}`",
        f"- 생성 경로: `{OUTPUT_DIR.relative_to(ROOT)}`",
        "",
        "## 차트 목록",
        "",
    ]

    for table, chart_path in zip(tables, chart_paths):
        rel = chart_path.relative_to(ROOT)
        lines.extend([
            f"### {table['title']}",
            "",
            f"- 구분: `{table['section']}`",
            f"- 파일: `{rel}`",
            "",
            f"![{table['title']}]({rel.as_posix()})",
            "",
        ])

    return "\n".join(lines) + "\n"


def main():
    markdown_text = SOURCE_MD.read_text(encoding="utf-8")
    tables = extract_tables(markdown_text)
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    chart_paths = []
    for index, table in enumerate(tables, start=1):
        filename = f"{index:02d}_{slugify(table['section'])}_{slugify(table['title'])}.svg"
        chart_path = OUTPUT_DIR / filename
        chart_path.write_text(build_svg(table, index), encoding="utf-8")
        chart_paths.append(chart_path)

    INDEX_MD.write_text(build_index(tables, chart_paths), encoding="utf-8")
    print(f"generated {len(chart_paths)} chart(s)")
    for chart_path in chart_paths:
        print(chart_path.relative_to(ROOT))
    print(INDEX_MD.relative_to(ROOT))


if __name__ == "__main__":
    main()
