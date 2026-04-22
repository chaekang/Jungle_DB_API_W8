#!/usr/bin/env python3
import json
import statistics
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RESULT_ROOT = ROOT / "output" / "queue-experiments-simwork"
OUTPUT_MD = ROOT / "docs" / "queue_size_simulated_work_results_20260422.md"


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
        "queue": int(server["queue_capacity"]),
        "simulate_work_us": int(server.get("simulated_work_us", "0")),
        "completed": int(server["total_requests_completed"]),
        "failed": int(server["total_requests_failed"]),
        "resp_503": int(server["total_503_responses"]),
        "avg_queue_wait_ms": float(server["average_queue_wait_ms"]),
        "avg_total_response_ms": float(server["average_total_response_ms"]),
        "avg_execution_ms": float(server["average_execution_ms"]),
        "worker_busy_ratio": float(server["worker_busy_ratio"]),
        "k6_p95_ms": float(k6["metrics"]["http_req_duration"]["p(95)"]),
        "workload": run_dir.name.split("-q", 1)[0],
        "rate": int(run_dir.name.split("-r", 1)[1].split("-", 1)[0]),
    }


def collect_runs():
    runs = []
    for run_dir in sorted(RESULT_ROOT.glob("*")):
        if run_dir.is_dir():
            run = load_run(run_dir)
            if run:
                runs.append(run)
    return runs


def median(values):
    return statistics.median(values)


def fmt_int(value):
    return f"{int(round(value)):,}"


def summarize(runs, workload, rate, simulate_work_us):
    rows = []
    subset = [
        run for run in runs
        if run["workload"] == workload and run["rate"] == rate and run["simulate_work_us"] == simulate_work_us
    ]
    for queue in [32, 48, 64]:
        qruns = [run for run in subset if run["queue"] == queue]
        if not qruns:
            continue
        rows.append({
            "queue": queue,
            "completed": median([run["completed"] for run in qruns]),
            "failed": median([run["failed"] for run in qruns]),
            "resp_503": median([run["resp_503"] for run in qruns]),
            "avg_queue_wait_ms": median([run["avg_queue_wait_ms"] for run in qruns]),
            "avg_total_response_ms": median([run["avg_total_response_ms"] for run in qruns]),
            "avg_execution_ms": median([run["avg_execution_ms"] for run in qruns]),
            "worker_busy_ratio": median([run["worker_busy_ratio"] for run in qruns]),
            "k6_p95_ms": median([run["k6_p95_ms"] for run in qruns]),
        })
    return rows


def make_table(rows):
    lines = [
        "| queue | median completed | median failed | median 503 | median avg queue wait(ms) | median avg total response(ms) | median avg execution(ms) | median worker busy ratio | median k6 p95 |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for row in rows:
        lines.append(
            f"| {row['queue']} | {fmt_int(row['completed'])} | {fmt_int(row['failed'])} | {fmt_int(row['resp_503'])} | "
            f"{row['avg_queue_wait_ms']:.3f} | {row['avg_total_response_ms']:.3f} | "
            f"{row['avg_execution_ms']:.3f} | {row['worker_busy_ratio']:.4f} | {row['k6_p95_ms']:.3f}ms |"
        )
    return lines


def group_note(workload, simulate_work_us):
    notes = {
        ("read_heavy", 0): "`48` 이 가장 설득력 있는 절충점이다. `64` 는 실패는 더 적지만 p95 가 다시 길어진다.",
        ("read_heavy", 100): "`48` 이 여전히 가장 균형적이다. `64` 는 503 은 0 이지만 p95 이점이 없다.",
        ("read_heavy", 250): "`48` 이 가장 설명하기 쉽다. `64` 는 실패를 더 줄이지만 p95 증가가 더 크다.",
        ("read_heavy", 500): "`64` 쪽으로 기운다. `48` 과 p95 는 비슷하지만 실패가 더 적다.",
        ("mixed", 0): "`64` 가 실패와 p95 를 동시에 가장 잘 잡았다.",
        ("mixed", 100): "`48` 이 가장 균형적이다. `64` 는 실패는 조금 더 적지만 p95 손해가 너무 크다.",
        ("mixed", 250): "`64` 쪽으로 기운다. `32` 는 너무 많이 실패하고, `48` 보다 `64` 가 실패와 p95 둘 다 낫다.",
        ("mixed", 500): "`64` 가 가장 실전적이다. 실패가 가장 적고 p95 도 가장 낮다.",
    }
    return notes.get((workload, simulate_work_us))


def build_markdown(runs):
    lines = [
        "# Queue Size Experiment With Simulated Work",
        "",
        "가설 검증 목적: 현재 `SELECT` / `INSERT` 가 너무 가벼워 queue size 차이가 잡음에 묻힐 수 있는지 확인하기 위해, worker가 SQL 실행 직전에 인위적인 작업 시간을 추가한 실험이다.",
        "",
        "## 실험 조건",
        "",
        "- worker thread: `8` 고정",
        "- queue 후보: `32 / 48 / 64`",
        "- workload: `read-heavy 3000 rps`, `mixed 5000 rps`",
        "- simulate work: `0 / 100 / 250 / 500 us`",
        "- 각 조건 3회 반복, 순서 랜덤화",
        "- 구현 위치: `server_simulate_work(simulated_work_us)` in `src/server.c`",
        "",
        "## 요약 해석",
        "",
        "- `simulate_work_us=0` 은 현재 실제 코드 경로의 baseline 이다.",
        "- `simulate_work_us` 가 커질수록 worker busy ratio 가 올라가면, 기존 workload가 너무 가벼웠다는 가설을 지지한다.",
        "- queue 기본값은 각 simulate 단계에서 `503` 감소와 `p95` 증가 사이 균형으로 본다.",
        "",
    ]

    for workload, rate, title in [
        ("read_heavy", 3000, "read-heavy"),
        ("mixed", 5000, "mixed"),
    ]:
        lines.extend([
            f"## {title}",
            "",
        ])
        for simulate_work_us in [0, 100, 250, 500]:
            rows = summarize(runs, workload, rate, simulate_work_us)
            lines.extend([
                f"### simulate work `{simulate_work_us} us`",
                "",
            ])
            lines.extend(make_table(rows))
            note = group_note(workload, simulate_work_us)
            if note is not None:
                lines.extend([
                    "",
                    f"- 해석: {note}",
                    "",
                ])
            else:
                lines.append("")

    lines.extend([
        "## 종합 결론",
        "",
        "1. `simulate_work_us` 가 커질수록 worker busy ratio 가 read-heavy `~1% -> ~19%`, mixed `~2~3% -> ~35%` 로 올라갔다.",
        "2. 따라서 기존 baseline workload 는 실제로 너무 가벼웠고, queue size 차이가 잡음과 섞였을 가능성이 높다는 가설이 지지된다.",
        "3. 가벼운 baseline 또는 read-heavy 성격이 강하면 `48` 이 여전히 좋은 후보다.",
        "4. mixed 비중이 높거나 실제 SQL 비용이 더 무거워지면 `64` 가 더 안전한 기본값으로 이동한다.",
        "5. 실서비스 기본값을 하나만 고르면, 현재는 `64` 가 더 보수적이고 방어적인 선택이다.",
        "",
    ])

    lines.extend([
        "## 원시 run 목록",
        "",
        "| run | workload | simulate work(us) | queue | completed | failed | 503 | avg execution(ms) | worker busy ratio | k6 p95 |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ])

    for run in sorted(runs, key=lambda item: item["name"]):
        lines.append(
            f"| `{run['name']}` | {run['workload']} | {run['simulate_work_us']} | {run['queue']} | "
            f"{fmt_int(run['completed'])} | {fmt_int(run['failed'])} | {fmt_int(run['resp_503'])} | "
            f"{run['avg_execution_ms']:.3f} | {run['worker_busy_ratio']:.4f} | {run['k6_p95_ms']:.3f}ms |"
        )

    lines.append("")
    return "\n".join(lines)


def main():
    runs = collect_runs()
    OUTPUT_MD.write_text(build_markdown(runs), encoding="utf-8")
    print(f"wrote {OUTPUT_MD.relative_to(ROOT)} with {len(runs)} runs")


if __name__ == "__main__":
    main()
