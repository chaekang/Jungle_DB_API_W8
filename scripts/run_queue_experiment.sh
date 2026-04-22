#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

PORT="${PORT:-18080}"
WORKERS="${WORKERS:-8}"
QUEUE_SIZE="${QUEUE_SIZE:-32}"
WORKLOAD="${WORKLOAD:-read_heavy}"
RATE="${RATE:-200}"
DURATION="${DURATION:-20s}"
SEED_ROWS="${SEED_ROWS:-10000}"
SEED_VUS="${SEED_VUS:-10}"
SEED_MAX_DURATION="${SEED_MAX_DURATION:-10m}"
PRE_ALLOCATED_VUS="${PRE_ALLOCATED_VUS:-200}"
MAX_VUS="${MAX_VUS:-1000}"
RESULT_ROOT="${RESULT_ROOT:-output/queue-experiments}"
SIMULATED_WORK_US="${SIMULATED_WORK_US:-0}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --port)
      PORT="$2"
      shift 2
      ;;
    --workers)
      WORKERS="$2"
      shift 2
      ;;
    --queue-size)
      QUEUE_SIZE="$2"
      shift 2
      ;;
    --workload)
      WORKLOAD="$2"
      shift 2
      ;;
    --rate)
      RATE="$2"
      shift 2
      ;;
    --duration)
      DURATION="$2"
      shift 2
      ;;
    --seed-rows)
      SEED_ROWS="$2"
      shift 2
      ;;
    --seed-vus)
      SEED_VUS="$2"
      shift 2
      ;;
    --seed-max-duration)
      SEED_MAX_DURATION="$2"
      shift 2
      ;;
    --pre-allocated-vus)
      PRE_ALLOCATED_VUS="$2"
      shift 2
      ;;
    --max-vus)
      MAX_VUS="$2"
      shift 2
      ;;
    --result-root)
      RESULT_ROOT="$2"
      shift 2
      ;;
    --simulate-work-us)
      SIMULATED_WORK_US="$2"
      shift 2
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

case "$WORKLOAD" in
  read_heavy|mixed)
    ;;
  *)
    echo "Unsupported WORKLOAD: $WORKLOAD" >&2
    exit 1
    ;;
esac

timestamp="$(date +%Y%m%d-%H%M%S)"
result_dir="$RESULT_ROOT/${WORKLOAD}-q${QUEUE_SIZE}-r${RATE}-${timestamp}"
mkdir -p "$result_dir"

server_log="$result_dir/server.log"
seed_log="$result_dir/seed.log"
load_log="$result_dir/load.log"
seed_summary="$result_dir/seed-summary.json"
load_summary="$result_dir/k6-summary.json"
server_summary="$result_dir/server-summary.txt"

server_pid=""

cleanup() {
  if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
    kill -INT "$server_pid" || true
    wait "$server_pid" || true
  fi
}

trap cleanup EXIT

server_cmd=(./sql_processor --server "$PORT" --workers "$WORKERS" --queue "$QUEUE_SIZE")
if [[ "$SIMULATED_WORK_US" -gt 0 ]]; then
  server_cmd+=(--simulate-work-us "$SIMULATED_WORK_US")
fi

"${server_cmd[@]}" >"$server_log" 2>&1 &
server_pid="$!"

for _ in $(seq 1 100); do
  if rg -q "Server listening" "$server_log"; then
    break
  fi
  if ! kill -0 "$server_pid" 2>/dev/null; then
    cat "$server_log" >&2
    exit 1
  fi
  sleep 0.1
done

if ! rg -q "Server listening" "$server_log"; then
  echo "Server did not start in time." >&2
  exit 1
fi

seed_attempt=1
seed_succeeded=0
while [[ "$seed_attempt" -le 3 ]]; do
  if k6 run --quiet \
    --summary-export "$seed_summary" \
    -e BASE_URL="http://127.0.0.1:${PORT}" \
    -e SEED_ROWS="$SEED_ROWS" \
    -e SEED_VUS="$SEED_VUS" \
    -e SEED_MAX_DURATION="$SEED_MAX_DURATION" \
    loadtest/k6/seed_bench_users.js >"$seed_log" 2>&1; then
    seed_succeeded=1
    break
  fi

  if [[ "$seed_attempt" -lt 3 ]]; then
    sleep 1
  fi
  seed_attempt=$((seed_attempt + 1))
done

if [[ "$seed_succeeded" -ne 1 ]]; then
  cat "$seed_log" >&2
  exit 1
fi

curl -sS -X POST "http://127.0.0.1:${PORT}/admin/reset-metrics" >/dev/null

k6 run --quiet \
  --summary-export "$load_summary" \
  -e BASE_URL="http://127.0.0.1:${PORT}" \
  -e ARRIVAL_RATE="$RATE" \
  -e TEST_DURATION="$DURATION" \
  -e SELECT_ID_MAX="$SEED_ROWS" \
  -e PRE_ALLOCATED_VUS="$PRE_ALLOCATED_VUS" \
  -e MAX_VUS="$MAX_VUS" \
  "loadtest/k6/${WORKLOAD}.js" >"$load_log" 2>&1

kill -INT "$server_pid"
wait "$server_pid" || true
server_pid=""

sed -n '/SERVER_SUMMARY_BEGIN/,/SERVER_SUMMARY_END/p' "$server_log" >"$server_summary"
printf '%s\n' "$result_dir"
