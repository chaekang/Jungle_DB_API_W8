#!/usr/bin/env python3
"""Benchmark indexed id SELECT against linear non-id field scans.

This script exercises the large-data performance condition from
docs/004-bplus-tree-index.md:

- load at least 1,000,000 records through INSERT by default
- measure SELECT by id through the mini DB B+Tree index path
- measure SELECT by another field through a fixed-row linear scan
- compare both elapsed times

Runtime requirement: Python 3 standard library only. No venv or third-party
package installation is required.
"""

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path


FIXED_ROW_SIZE = 64
FIXED_ROW_DATA_SIZE = FIXED_ROW_SIZE - 1
FIXED_ROW_PADDING = "_"
DEFAULT_RECORDS = 1000000
DEFAULT_SELECT_REPETITIONS = 50

REPO_ROOT = Path(__file__).resolve().parents[1]
DATA_DIR = REPO_ROOT / "data"
USERS_CSV = DATA_DIR / "users.csv"
USERS_INDEX = DATA_DIR / "users.idx"
USERS_INDEX_BOOT = DATA_DIR / "users.idx.boot"
DEFAULT_EXECUTABLE = REPO_ROOT / "cmake-build-debug" / "jungle_mini_db"


class BenchmarkResult:
    def __init__(
        self,
        records,
        select_repetitions,
        target_id,
        target_name,
        load_mode,
        load_seconds,
        indexed_select_seconds,
        linear_select_seconds,
    ):
        self.records = records
        self.select_repetitions = select_repetitions
        self.target_id = target_id
        self.target_name = target_name
        self.load_mode = load_mode
        self.load_seconds = load_seconds
        self.indexed_select_seconds = indexed_select_seconds
        self.linear_select_seconds = linear_select_seconds

    @property
    def indexed_avg_ms(self):
        return self.indexed_select_seconds * 1000 / self.select_repetitions

    @property
    def linear_avg_ms(self):
        return self.linear_select_seconds * 1000 / self.select_repetitions

    @property
    def speedup(self):
        if self.indexed_select_seconds == 0:
            return float("inf")
        return self.linear_select_seconds / self.indexed_select_seconds

    def to_dict(self):
        return {
            "records": self.records,
            "select_repetitions": self.select_repetitions,
            "target_id": self.target_id,
            "target_name": self.target_name,
            "load_mode": self.load_mode,
            "load_seconds": self.load_seconds,
            "indexed_select_seconds": self.indexed_select_seconds,
            "indexed_avg_ms": self.indexed_avg_ms,
            "linear_select_seconds": self.linear_select_seconds,
            "linear_avg_ms": self.linear_avg_ms,
            "linear_vs_indexed_speedup": self.speedup,
        }


class DataBackup:
    def __init__(self, paths):
        self._snapshots = {}
        for path in paths:
            self._snapshots[path] = path.read_bytes() if path.exists() else None

    def restore(self):
        for path, content in self._snapshots.items():
            if content is None:
                unlink_if_exists(path)
                continue

            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run the B+Tree index benchmark required by docs/004-bplus-tree-index.md."
    )
    parser.add_argument(
        "--records",
        type=int,
        default=DEFAULT_RECORDS,
        help="number of records to load; default: %d" % DEFAULT_RECORDS,
    )
    parser.add_argument(
        "--select-repetitions",
        type=int,
        default=DEFAULT_SELECT_REPETITIONS,
        help="number of repeated SELECT measurements; default: %d" % DEFAULT_SELECT_REPETITIONS,
    )
    parser.add_argument(
        "--target-id",
        type=int,
        default=None,
        help="id to select; default: the last generated id",
    )
    parser.add_argument(
        "--load-mode",
        choices=("insert", "generate"),
        default="insert",
        help="insert uses mini DB INSERT statements; generate writes fixed rows directly for faster smoke tests",
    )
    parser.add_argument(
        "--executable",
        type=Path,
        default=DEFAULT_EXECUTABLE,
        help="mini DB executable path; default: %s" % DEFAULT_EXECUTABLE,
    )
    parser.add_argument(
        "--skip-build",
        action="store_true",
        help="skip cmake build before running the benchmark",
    )
    parser.add_argument(
        "--keep-data",
        action="store_true",
        help="keep generated users.csv and index files after the benchmark",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="optional JSON output path for benchmark results",
    )
    return parser.parse_args()


def validate_args(args):
    if args.records < 1:
        raise ValueError("--records must be at least 1")
    if args.select_repetitions < 1:
        raise ValueError("--select-repetitions must be at least 1")
    if args.target_id is not None and not (1 <= args.target_id <= args.records):
        raise ValueError("--target-id must be between 1 and --records")

    if args.load_mode == "insert" and args.records < DEFAULT_RECORDS:
        print(
            "warning: docs/004 requires at least %d INSERTs; this run uses %d."
            % (DEFAULT_RECORDS, args.records),
            file=sys.stderr,
        )


def build_project():
    completed = subprocess.run(
        ["cmake", "--build", "cmake-build-debug"],
        cwd=REPO_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            "cmake build failed with exit code %d:\nstdout:\n%s\nstderr:\n%s"
            % (completed.returncode, completed.stdout, completed.stderr)
        )


def print_progress(message):
    print(message, file=sys.stderr, flush=True)


def unlink_if_exists(path):
    try:
        path.unlink()
    except FileNotFoundError:
        pass


def reset_users_table():
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    USERS_CSV.write_bytes(b"")
    unlink_if_exists(USERS_INDEX)
    unlink_if_exists(USERS_INDEX_BOOT)


def fixed_user_name(record_id):
    return "user-%07d" % record_id


def encode_fixed_row(record_id):
    logical_row = "%d,%s," % (record_id, fixed_user_name(record_id))
    if len(logical_row) > FIXED_ROW_DATA_SIZE:
        raise ValueError("record %d is too large for fixed row storage" % record_id)

    padded = logical_row + (FIXED_ROW_PADDING * (FIXED_ROW_DATA_SIZE - len(logical_row))) + "\n"
    return padded.encode("ascii")


def load_records_with_insert(executable, records):
    started = time.perf_counter()
    process = subprocess.Popen(
        [str(executable)],
        cwd=REPO_ROOT,
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )

    assert process.stdin is not None
    for start_id in range(1, records + 1, 10000):
        end_id = min(start_id + 9999, records)
        commands = [
            "insert into users values (%d,%s);\n" % (record_id, fixed_user_name(record_id))
            for record_id in range(start_id, end_id + 1)
        ]
        process.stdin.write("".join(commands).encode("ascii"))
        process.stdin.flush()
        if records >= 100000 and (end_id % 100000 == 0 or end_id == records):
            print_progress("  inserted {:,}/{:,} records".format(end_id, records))

    process.stdin.write(b".exit\n")
    process.stdin.close()
    return_code = process.wait()
    stderr = process.stderr.read().decode("utf-8", errors="replace") if process.stderr else ""
    if return_code != 0:
        raise RuntimeError("mini DB INSERT load failed with exit code %d:\n%s" % (return_code, stderr))

    return time.perf_counter() - started


def load_records_by_generation(executable, records):
    started = time.perf_counter()
    with USERS_CSV.open("wb") as file:
        for record_id in range(1, records + 1):
            file.write(encode_fixed_row(record_id))

    run_mini_db_batch(executable, ".exit\n")
    return time.perf_counter() - started


def run_mini_db_batch(executable, commands):
    started = time.perf_counter()
    completed = subprocess.run(
        [str(executable)],
        cwd=REPO_ROOT,
        input=commands.encode("ascii"),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode != 0:
        stderr = completed.stderr.decode("utf-8", errors="replace")
        raise RuntimeError(
            "mini DB command batch failed with exit code %d:\n%s" % (completed.returncode, stderr)
        )
    return time.perf_counter() - started


def measure_indexed_select(executable, target_id, repetitions):
    commands = "".join(
        "select * from users where id = %d;\n" % target_id for _ in range(repetitions)
    )
    return run_mini_db_batch(executable, commands + ".exit\n")


def decode_fixed_row(raw_row):
    if len(raw_row) != FIXED_ROW_SIZE or raw_row[-1:] != b"\n":
        raise ValueError("invalid fixed row")

    return raw_row[:FIXED_ROW_DATA_SIZE].decode("ascii").rstrip(FIXED_ROW_PADDING).rstrip(",")


def linear_select_by_name(target_name):
    with USERS_CSV.open("rb") as file:
        while True:
            raw_row = file.read(FIXED_ROW_SIZE)
            if raw_row == b"":
                break
            logical_row = decode_fixed_row(raw_row)
            columns = logical_row.split(",")
            if len(columns) >= 2 and columns[1] == target_name:
                return logical_row

    raise LookupError("name not found by linear scan: %s" % target_name)


def measure_linear_select(target_name, repetitions):
    started = time.perf_counter()
    for _ in range(repetitions):
        linear_select_by_name(target_name)
    return time.perf_counter() - started


def write_output(path, result):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(result.to_dict(), indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )


def print_result(result):
    print("B+Tree index benchmark")
    print("- records loaded: {:,}".format(result.records))
    print("- load mode: %s" % result.load_mode)
    print("- target: id=%d, name=%s" % (result.target_id, result.target_name))
    print("- select repetitions: {:,}".format(result.select_repetitions))
    print("- load time: %.3fs" % result.load_seconds)
    print(
        "- indexed id SELECT: "
        "%.6fs total, %.3fms/query" % (result.indexed_select_seconds, result.indexed_avg_ms)
    )
    print(
        "- linear name SELECT: "
        "%.6fs total, %.3fms/query" % (result.linear_select_seconds, result.linear_avg_ms)
    )
    print("- linear/indexed elapsed ratio: %.2fx" % result.speedup)
    print()
    print("note: indexed SELECT is measured through the mini DB SQL path.")
    print("note: non-id SELECT is measured by scanning fixed-row users.csv linearly because SQL only supports where id.")


def main():
    args = parse_args()
    try:
        validate_args(args)
    except ValueError as error:
        print("error: %s" % error, file=sys.stderr)
        return 2

    executable = args.executable.resolve()
    backup = DataBackup([USERS_CSV, USERS_INDEX, USERS_INDEX_BOOT])

    try:
        if not args.skip_build:
            print_progress("[1/4] building mini DB")
            build_project()
            print_progress("[1/4] build complete")
        if not executable.exists():
            raise FileNotFoundError("mini DB executable not found: %s" % executable)

        target_id = args.target_id if args.target_id is not None else args.records
        target_name = fixed_user_name(target_id)

        print_progress("[2/4] preparing users table")
        reset_users_table()
        print_progress("[3/4] loading {:,} records using {} mode".format(args.records, args.load_mode))
        if args.load_mode == "insert":
            load_seconds = load_records_with_insert(executable, args.records)
        else:
            load_seconds = load_records_by_generation(executable, args.records)

        print_progress("[4/4] measuring SELECT performance")
        indexed_seconds = measure_indexed_select(executable, target_id, args.select_repetitions)
        linear_seconds = measure_linear_select(target_name, args.select_repetitions)

        result = BenchmarkResult(
            records=args.records,
            select_repetitions=args.select_repetitions,
            target_id=target_id,
            target_name=target_name,
            load_mode=args.load_mode,
            load_seconds=load_seconds,
            indexed_select_seconds=indexed_seconds,
            linear_select_seconds=linear_seconds,
        )
        print_result(result)
        if args.output is not None:
            write_output(args.output, result)
            print_progress("wrote JSON result: %s" % args.output)

        return 0
    except Exception as error:
        print("error: %s" % error, file=sys.stderr)
        return 1
    finally:
        if not args.keep_data:
            backup.restore()


if __name__ == "__main__":
    raise SystemExit(main())
