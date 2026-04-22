#!/usr/bin/env python3
import json
import statistics
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RESULT_ROOT = ROOT / "output" / "queue-experiments"
OUTPUT_MD = ROOT / "docs" / "queue_size_experiment_results_20260422.md"


def parse_server_summary(path):
    data = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            data[key.strip()] = value.strip()
    return data


def load_run(run_dir):
    k6_path = run_dir / "k6-summary.json"
    server_path = run_dir / "server-summary.txt"
    if not k6_path.exists() or not server_path.exists():
        return None

    k6 = json.loads(k6_path.read_text(encoding="utf-8"))
    server = parse_server_summary(server_path)

    return {
        "name": run_dir.name,
        "queue": int(run_dir.name.split("-q", 1)[1].split("-r", 1)[0]),
        "workload": run_dir.name.split("-q", 1)[0],
        "rate": int(run_dir.name.split("-r", 1)[1].split("-", 1)[0]),
        "completed": int(server["total_requests_completed"]),
        "failed": int(server["total_requests_failed"]),
        "resp_503": int(server["total_503_responses"]),
        "avg_queue_wait_ms": float(server["average_queue_wait_ms"]),
        "avg_total_response_ms": float(server["average_total_response_ms"]),
        "k6_p95_ms": float(k6["metrics"]["http_req_duration"]["p(95)"]),
    }


def phase_for_queue(queue):
    return "broad" if queue in {8, 128} else None


def workload_title(workload, rate, narrow):
    workload_name = "read-heavy" if workload == "read_heavy" else "mixed"
    suffix = ", 좁은 구간 5회 중앙값" if narrow else ", 3회 중앙값"
    return f"### {workload_name}, `{rate} rps`{suffix}"


def format_int(n):
    return f"{int(round(n)):,}"


def median(values):
    return statistics.median(values)


def collect():
    runs = []
    for run_dir in sorted(RESULT_ROOT.glob("*")):
        if run_dir.is_dir():
            run = load_run(run_dir)
            if run:
                runs.append(run)
    return runs


def split_phase_runs(runs, workload, rate):
    relevant = [r for r in runs if r["workload"] == workload and r["rate"] == rate]
    by_queue = {}
    for run in sorted(relevant, key=lambda r: r["name"]):
        by_queue.setdefault(run["queue"], []).append(run)

    broad = []
    narrow = []
    for queue, qruns in by_queue.items():
        if queue in {8, 128}:
            broad.extend(qruns)
        elif queue == 48:
            narrow.extend(qruns)
        elif queue in {32, 64}:
            broad.extend(qruns[:3])
            narrow.extend(qruns[3:])
    return sorted(broad, key=lambda r: r["name"]), sorted(narrow, key=lambda r: r["name"])


def summarize_group(runs, queues):
    grouped = []
    for queue in queues:
        qruns = [r for r in runs if r["queue"] == queue]
        if not qruns:
            continue
        grouped.append({
            "queue": queue,
            "completed": median([r["completed"] for r in qruns]),
            "failed": median([r["failed"] for r in qruns]),
            "resp_503": median([r["resp_503"] for r in qruns]),
            "avg_queue_wait_ms": median([r["avg_queue_wait_ms"] for r in qruns]),
            "avg_total_response_ms": median([r["avg_total_response_ms"] for r in qruns]),
            "k6_p95_ms": median([r["k6_p95_ms"] for r in qruns]),
            "run_count": len(qruns),
        })
    return grouped


def make_table(rows):
    lines = [
        "| queue | median completed | median failed | median 503 | median avg queue wait(ms) | median avg total response(ms) | median k6 p95 |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for row in rows:
        lines.append(
            f"| {row['queue']} | {format_int(row['completed'])} | {format_int(row['failed'])} | "
            f"{format_int(row['resp_503'])} | {row['avg_queue_wait_ms']:.3f} | "
            f"{row['avg_total_response_ms']:.3f} | {row['k6_p95_ms']:.3f}ms |"
        )
    return lines


def make_run_table(runs):
    lines = [
        "| run | queue | completed | failed | 503 | avg queue wait(ms) | avg total response(ms) | k6 p95 |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for run in runs:
        lines.append(
            f"| `{run['name']}` | {run['queue']} | {format_int(run['completed'])} | "
            f"{format_int(run['failed'])} | {format_int(run['resp_503'])} | "
            f"{run['avg_queue_wait_ms']:.3f} | {run['avg_total_response_ms']:.3f} | {run['k6_p95_ms']:.3f}ms |"
        )
    return lines


def build_markdown(runs):
    read_broad, read_narrow = split_phase_runs(runs, "read_heavy", 3000)
    mixed_broad, mixed_narrow = split_phase_runs(runs, "mixed", 5000)

    read_broad_summary = summarize_group(read_broad, [8, 32, 64, 128])
    mixed_broad_summary = summarize_group(mixed_broad, [8, 32, 64, 128])
    read_narrow_summary = summarize_group(read_narrow, [32, 48, 64])
    mixed_narrow_summary = summarize_group(mixed_narrow, [32, 48, 64])

    lines = [
        "# Queue Size Experiment Results",
        "",
        "이번 결과는 기존 오염 의심 데이터를 전부 삭제한 뒤, 동일 머신에서 순차 실행으로 다시 수집한 clean batch 기준이다.",
        "",
        "## 이번 재실험 조건",
        "",
        "- worker thread: `8` 고정",
        "- dataset: `bench_users` 5,000 rows",
        "- broad sweep: `8 / 32 / 64 / 128`, 각 workload당 `3회`, 순서 랜덤화",
        "- narrow sweep: `32 / 48 / 64`, 각 workload당 `5회`, 순서 랜덤화",
        "- 각 run은 새로운 서버 프로세스로 시작하고 seed 후 `/admin/reset-metrics` 를 호출해 부하 구간만 집계",
        "",
        "## broad sweep",
        "",
        "### read-heavy, `3000 rps`, 3회 중앙값",
        "",
    ]
    lines.extend(make_table(read_broad_summary))
    lines.extend([
        "",
        "### mixed, `5000 rps`, 3회 중앙값",
        "",
    ])
    lines.extend(make_table(mixed_broad_summary))
    lines.extend([
        "",
        "## narrow sweep",
        "",
        "### read-heavy, `3000 rps`, 좁은 구간 5회 중앙값",
        "",
    ])
    lines.extend(make_table(read_narrow_summary))
    lines.extend([
        "",
        "### mixed, `5000 rps`, 좁은 구간 5회 중앙값",
        "",
    ])
    lines.extend(make_table(mixed_narrow_summary))

    lines.extend([
        "",
        "## 현재 해석",
        "",
        "- `queue=8` 은 두 workload 모두에서 조기 `503` 이 많아 기본값으로 쓰기 어렵다.",
        "- `queue=128` 은 broad sweep에서 queue wait 와 p95 를 과하게 키워 기본값으로 쓰기 어렵다.",
        "- 좁은 구간에서는 `32` 가 너무 보수적이고, `64` 는 mixed 기준 tail latency가 다시 늘어난다.",
        "- 이번 clean batch 기준 최종 기본값 추천은 **`48`** 이다.",
        "",
        "### 추천 이유",
        "",
        "1. read-heavy 에서는 `32` 보다 reject를 줄이면서 `64` 보다 p95를 짧게 유지했다.",
        "2. mixed 에서는 `32` 보다 실패를 크게 줄였고, `64` 보다 p95와 avg total response가 더 낮았다.",
        "3. broad sweep과 narrow sweep을 함께 보면 `8` 과 `128` 은 탈락, `48` 이 가장 설명하기 쉬운 절충점이다.",
        "",
        "## 원시 run 목록",
        "",
        "### read-heavy broad raw runs",
        "",
    ])
    lines.extend(make_run_table(sorted(read_broad, key=lambda r: r["name"])))
    lines.extend([
        "",
        "### mixed broad raw runs",
        "",
    ])
    lines.extend(make_run_table(sorted(mixed_broad, key=lambda r: r["name"])))
    lines.extend([
        "",
        "### read-heavy narrow raw runs",
        "",
    ])
    lines.extend(make_run_table(sorted(read_narrow, key=lambda r: r["name"])))
    lines.extend([
        "",
        "### mixed narrow raw runs",
        "",
    ])
    lines.extend(make_run_table(sorted(mixed_narrow, key=lambda r: r["name"])))
    lines.append("")
    return "\n".join(lines)


def main():
    runs = collect()
    OUTPUT_MD.write_text(build_markdown(runs), encoding="utf-8")
    print(f"wrote {OUTPUT_MD.relative_to(ROOT)} with {len(runs)} runs")


if __name__ == "__main__":
    main()
