#!/usr/bin/env python3
import argparse
import random
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "scripts" / "run_queue_experiment.sh"


def build_jobs():
    jobs = []
    specs = [
        ("read_heavy", 3000),
        ("mixed", 5000),
    ]
    queue_sizes = [32, 48, 64]
    simulate_values = [0, 100, 250, 500]
    repeats = 3

    for workload, rate in specs:
        for repeat in range(1, repeats + 1):
            combos = []
            for simulate_work_us in simulate_values:
                for queue in queue_sizes:
                    combos.append({
                        "workload": workload,
                        "rate": rate,
                        "queue": queue,
                        "simulate_work_us": simulate_work_us,
                        "repeat": repeat,
                    })
            rng = random.Random(f"{workload}-{rate}-{repeat}-simwork")
            rng.shuffle(combos)
            jobs.extend(combos)
    return jobs


def run_job(job, port, result_root):
    cmd = [
        str(RUNNER),
        "--port", str(port),
        "--workers", "8",
        "--queue-size", str(job["queue"]),
        "--workload", job["workload"],
        "--rate", str(job["rate"]),
        "--duration", "10s",
        "--seed-rows", "5000",
        "--seed-vus", "2",
        "--seed-max-duration", "10m",
        "--pre-allocated-vus", "1000",
        "--max-vus", "4000",
        "--result-root", result_root,
        "--simulate-work-us", str(job["simulate_work_us"]),
    ]
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)


def main():
    parser = argparse.ArgumentParser(description="Run queue experiment campaign with simulated work.")
    parser.add_argument("--start-port", type=int, default=18200)
    parser.add_argument("--result-root", default="output/queue-experiments-simwork")
    args = parser.parse_args()

    jobs = build_jobs()
    failures = []
    print(f"running {len(jobs)} jobs", flush=True)

    for index, job in enumerate(jobs, start=1):
        port = args.start_port + (index % 20)
        desc = (
            f"[{index}/{len(jobs)}] workload={job['workload']} rate={job['rate']} "
            f"queue={job['queue']} simulate={job['simulate_work_us']}us repeat={job['repeat']} port={port}"
        )
        print(desc, flush=True)
        result = run_job(job, port, args.result_root)
        if result.returncode != 0:
            print(result.stdout, end="", flush=True)
            print(result.stderr, end="", file=sys.stderr, flush=True)
            failures.append(desc)
            break
        output = result.stdout.strip().splitlines()
        if output:
            print(f"  result_dir={output[-1]}", flush=True)

    if failures:
        print("campaign failed", file=sys.stderr)
        for failure in failures:
            print(failure, file=sys.stderr)
        sys.exit(1)

    print("campaign completed", flush=True)


if __name__ == "__main__":
    main()
