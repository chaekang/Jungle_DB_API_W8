#!/usr/bin/env python3
import html
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "docs" / "assets" / "queue_size_presentation"
INDEX_MD = ROOT / "docs" / "queue_size_presentation_visuals.md"


DATASETS = [
    {
        "slug": "01_small_queue_burst_failure",
        "title": "포인트 1. 작은 큐는 burst를 흡수하지 못하고 너무 빨리 503을 낸다",
        "subtitle": "mixed workload, 5000 rps, broad sweep",
        "message": [
            "queue=8 은 지나치게 작은 큐였다.",
            "queue=32 로만 올려도 503 이 1,714 -> 959 로 크게 줄었다.",
            "즉 작은 큐의 핵심 문제는 latency보다 조기 reject 다.",
        ],
        "panels": [
            {
                "title": "503 count",
                "unit": "count",
                "series": [(8, 1714), (32, 959), (64, 845), (128, 314)],
                "highlight": 8,
            },
            {
                "title": "completed requests",
                "unit": "count",
                "series": [(8, 48287), (32, 49042), (64, 49155), (128, 49687)],
                "highlight": 8,
            },
        ],
    },
    {
        "slug": "02_large_queue_tail_latency",
        "title": "포인트 2. 큰 큐는 실패를 줄이지만 tail latency를 늘린다",
        "subtitle": "read-heavy workload, 3000 rps, broad sweep",
        "message": [
            "queue=128 은 503 을 줄였지만 p95 와 queue wait 가 동시에 증가했다.",
            "q64 -> q128 에서 avg queue wait 는 0.026 -> 0.383 ms 로 늘었다.",
            "즉 큰 큐는 늦게 실패하는 서버를 만들 위험이 있다.",
        ],
        "panels": [
            {
                "title": "k6 p95 latency",
                "unit": "ms",
                "series": [(8, 0.177), (32, 0.184), (64, 0.261), (128, 1.868)],
                "highlight": 128,
            },
            {
                "title": "avg queue wait",
                "unit": "ms",
                "series": [(8, 0.015), (32, 0.028), (64, 0.026), (128, 0.383)],
                "highlight": 128,
            },
        ],
    },
    {
        "slug": "03_final_candidate_read_heavy",
        "title": "포인트 3. read-heavy 에서는 48 이 32 와 64 사이의 절충점이다",
        "subtitle": "read-heavy workload, 3000 rps, narrow median of 5 runs",
        "message": [
            "q32 는 p95 는 짧지만 reject 가 여전히 많았다.",
            "q64 는 reject 는 더 적지만 p95 가 48 보다 더 길었다.",
            "q48 은 reject 와 tail latency 사이에서 가장 설명하기 쉬운 중간값이었다.",
        ],
        "panels": [
            {
                "title": "503 count",
                "unit": "count",
                "series": [(32, 162), (48, 64), (64, 30)],
                "highlight": 48,
            },
            {
                "title": "k6 p95 latency",
                "unit": "ms",
                "series": [(32, 0.214), (48, 0.369), (64, 0.407)],
                "highlight": 48,
            },
        ],
    },
    {
        "slug": "04_final_candidate_mixed",
        "title": "포인트 4. mixed 에서도 48 이 32 와 64 사이의 가장 설득력 있는 절충점이다",
        "subtitle": "mixed workload, 5000 rps, narrow median of 5 runs",
        "message": [
            "q32 는 503 이 너무 많았다.",
            "q64 는 q48 보다 503 은 조금 적지만 p95 와 total response 가 더 길었다.",
            "q48 은 reject 와 latency 사이 균형점으로 설명하기 가장 쉽다.",
        ],
        "panels": [
            {
                "title": "503 count",
                "unit": "count",
                "series": [(32, 839), (48, 492), (64, 435)],
                "highlight": 48,
            },
            {
                "title": "k6 p95 latency",
                "unit": "ms",
                "series": [(32, 0.373), (48, 0.366), (64, 0.497)],
                "highlight": 48,
            },
        ],
    },
]


def esc(text):
    return html.escape(str(text))


def svg_text(x, y, text, size=16, weight="400", fill="#111827", anchor="start"):
    return (
        f'<text x="{x}" y="{y}" font-size="{size}" font-weight="{weight}" fill="{fill}" '
        f'text-anchor="{anchor}" font-family="Pretendard, Apple SD Gothic Neo, Noto Sans CJK KR, sans-serif">{esc(text)}</text>'
    )


def svg_rect(x, y, w, h, fill, rx=14, stroke="none", stroke_width=0):
    return (
        f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{fill}" '
        f'stroke="{stroke}" stroke-width="{stroke_width}" />'
    )


def format_value(value, unit):
    if unit == "count":
        return f"{int(round(value)):,}"
    return f"{value:.3f} ms"


def palette(queue, highlight):
    if queue == highlight:
        return "#EA580C"
    colors = {
        8: "#94A3B8",
        32: "#2563EB",
        48: "#0F766E",
        64: "#059669",
        128: "#7C3AED",
    }
    return colors.get(queue, "#475569")


def draw_panel(x, y, w, h, panel):
    title = panel["title"]
    unit = panel["unit"]
    series = panel["series"]
    highlight = panel["highlight"]

    parts = [svg_rect(x, y, w, h, "#FFFFFF", rx=18, stroke="#E2E8F0", stroke_width=1)]
    parts.append(svg_text(x + 24, y + 34, title, size=20, weight="700"))

    chart_x = x + 24
    chart_y = y + 60
    chart_w = w - 48
    chart_h = h - 100
    max_value = max(value for _, value in series)
    if max_value <= 0:
        max_value = 1

    count = len(series)
    gap = 24
    bar_w = (chart_w - gap * (count + 1)) / count

    parts.append(f'<line x1="{chart_x}" y1="{chart_y + chart_h}" x2="{chart_x + chart_w}" y2="{chart_y + chart_h}" stroke="#CBD5E1" stroke-width="1.5" />')
    parts.append(svg_text(chart_x, chart_y + chart_h + 22, "0", size=12, fill="#64748B"))
    parts.append(svg_text(chart_x + chart_w, chart_y + 14, format_value(max_value, unit), size=12, fill="#64748B", anchor="end"))

    for idx, (queue, value) in enumerate(series):
        ratio = value / max_value if max_value else 0
        bar_h = max(chart_h * ratio, 3)
        bx = chart_x + gap + idx * (bar_w + gap)
        by = chart_y + chart_h - bar_h
        color = palette(queue, highlight)
        parts.append(svg_rect(bx, by, bar_w, bar_h, color, rx=10))
        parts.append(svg_text(bx + bar_w / 2, chart_y + chart_h + 18, f"q{queue}", size=13, weight="700", fill="#334155", anchor="middle"))
        parts.append(svg_text(bx + bar_w / 2, by - 10, format_value(value, unit), size=13, weight="700", fill="#0F172A", anchor="middle"))

        if queue == highlight:
            parts.append(svg_text(bx + bar_w / 2, by - 28, "focus", size=11, weight="700", fill="#EA580C", anchor="middle"))

    return "\n".join(parts)


def build_svg(spec):
    width = 1600
    height = 900
    left = 72
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#F8FAFC" />',
        svg_text(left, 70, spec["title"], size=34, weight="800", fill="#0F172A"),
        svg_text(left, 108, spec["subtitle"], size=18, weight="600", fill="#1D4ED8"),
        svg_rect(left, 140, 1456, 118, "#EFF6FF", rx=20, stroke="#BFDBFE", stroke_width=1),
    ]

    message_y = 178
    for line in spec["message"]:
        parts.append(svg_text(left + 24, message_y, f"- {line}", size=20, fill="#1E293B"))
        message_y += 30

    parts.append(draw_panel(72, 300, 700, 520, spec["panels"][0]))
    parts.append(draw_panel(828, 300, 700, 520, spec["panels"][1]))
    parts.append(svg_text(left, 860, "Source: docs/queue_size_experiment_results_20260422.md", size=14, fill="#64748B"))
    parts.append("</svg>")
    return "\n".join(parts)


def build_index(paths):
    lines = [
        "# Queue Size Presentation Visuals",
        "",
        "발표에서 바로 사용할 핵심 메시지만 남긴 요약 시각화다.",
        "",
        "## 발표 포인트",
        "",
        "1. 작은 큐는 burst를 흡수하지 못하고 조기 503을 많이 낸다.",
        "2. 큰 큐는 실패를 줄이는 대신 tail latency와 queue wait를 늘린다.",
        "3. 현재 조건에서는 `48` 이 `32` 와 `64` 사이의 가장 설득력 있는 절충점이다.",
        "",
        "## 이미지",
        "",
    ]

    for spec, path in zip(DATASETS, paths):
        rel = path.relative_to(ROOT).as_posix()
        lines.extend([
            f"### {spec['title']}",
            "",
            f"![{spec['title']}]({rel})",
            "",
        ])

    return "\n".join(lines) + "\n"


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    paths = []
    for spec in DATASETS:
        path = OUT_DIR / f"{spec['slug']}.svg"
        path.write_text(build_svg(spec), encoding="utf-8")
        paths.append(path)
    INDEX_MD.write_text(build_index(paths), encoding="utf-8")
    print(f"generated {len(paths)} presentation visual(s)")
    for path in paths:
        print(path.relative_to(ROOT))
    print(INDEX_MD.relative_to(ROOT))


if __name__ == "__main__":
    main()
