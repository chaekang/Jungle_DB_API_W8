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

    broad_specs = [
        ("read_heavy", 3000, [8, 32, 64, 128], 3),
        ("mixed", 5000, [8, 32, 64, 128], 3),
    ]
    narrow_specs = [
        ("read_heavy", 3000, [32, 48, 64], 5),
        ("mixed", 5000, [32, 48, 64], 5),
    ]

    for phase, specs in (("broad", broad_specs), ("narrow", narrow_specs)):
        for workload, rate, queues, repeats in specs:
            for repeat in range(1, repeats + 1):
                shuffled = list(queues)
                rng = random.Random(f"{phase}-{workload}-{rate}-{repeat}")
                rng.shuffle(shuffled)
                for queue in shuffled:
                    jobs.append({
                        "phase": phase,
                        "workload": workload,
                        "rate": rate,
                        "queue": queue,
                        "repeat": repeat,
                    })

    return jobs


def run_job(job, port):
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
        "--result-root", "output/queue-experiments",
    ]
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)


def main():
    parser = argparse.ArgumentParser(description="Run clean queue experiment campaign.")
    parser.add_argument("--start-port", type=int, default=18100)
    args = parser.parse_args()

    jobs = build_jobs()
    total = len(jobs)

    print(f"running {total} jobs", flush=True)
    failures = []

    for index, job in enumerate(jobs, start=1):
        port = args.start_port + (index % 20)
        desc = (
            f"[{index}/{total}] phase={job['phase']} workload={job['workload']} "
            f"rate={job['rate']} queue={job['queue']} repeat={job['repeat']} port={port}"
        )
        print(desc, flush=True)
        result = run_job(job, port)
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
