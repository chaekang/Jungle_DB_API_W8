# Worker Count Benchmark Comparison

## 1. 목적

현재 프로젝트에서 `worker=1`일 때와 `worker>1`일 때 실제 성능 차이가 얼마나 나는지 확인한다.

이번 실험은 다음 질문에 답하는 것이 목적이다.

1. `read-heavy` 부하에서 worker 수를 늘리면 처리량이 실제로 증가하는가
2. `mixed` 부하에서 worker 수를 늘리면 latency와 `503`이 얼마나 줄어드는가
3. 현재 코드베이스에서 무조건 worker를 많이 두는 것이 유리한가

## 2. 실행 조건

실험은 현재 저장소의 기존 스크립트 `scripts/run_queue_experiment.sh` 를 사용해 실행했다.

고정 조건은 아래와 같다.

| 항목 | 값 |
| --- | --- |
| queue size | `32` |
| seed rows | `5000` |
| 부하 도구 | `k6` |
| 서버 실행 파일 | `./sql_processor --server` |
| 비교 worker 수 | `1`, `2`, `4`, `8` |

워크로드는 아래 2개를 사용했다.

- `read_heavy`: `SELECT * FROM bench_users WHERE id = ?`
- `mixed`: `SELECT` 와 `INSERT` 혼합

비교에 사용한 대표 실험 조건은 아래와 같다.

| workload | rate | duration |
| --- | --- | --- |
| `read_heavy` | `3000 rps` | `10s` |
| `mixed` | `3000 rps` | `20s` |

## 3. 측정 지표

비교에는 아래 지표를 사용했다.

- `throughput_success_rps`
- `average_queue_wait_ms`
- `average_total_response_ms`
- `success_duration p95`
- `total_503_responses`

서버 내부 지표는 각 실험 디렉터리의 `server-summary.txt` 에서 가져왔고, latency 지표는 `k6-summary.json` 에서 가져왔다.

## 4. 결과

### 4.1 Read-heavy

조건: `read_heavy @ 3000 rps, 10s`

| workers | throughput (rps) | avg queue wait (ms) | avg response (ms) | p95 success (ms) | 503 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 2870.684 | 0.076 | 0.106 | 0.159 | 300 |
| 2 | 2894.732 | 0.019 | 0.047 | 0.156 | 82 |
| 4 | 2673.925 | 0.018 | 0.048 | 0.148 | 153 |
| 8 | 2852.914 | 0.017 | 0.047 | 0.142 | 145 |

해석:

- `read_heavy` 에서는 `worker=1` 과 multi-worker 사이의 처리량 차이가 거의 없다.
- 최고 처리량은 `workers=2` 에서 나왔지만 `worker=1` 대비 증가폭은 약 `0.8%` 수준이다.
- 반면 queue wait 과 응답시간은 multi-worker 에서 조금 더 안정적이다.
- 즉 현재 read path 는 이미 가벼워서 worker 수를 늘려도 큰 처리량 상승은 보이지 않는다.

### 4.2 Mixed

조건: `mixed @ 3000 rps, 20s`

| workers | throughput (rps) | avg queue wait (ms) | avg response (ms) | p95 success (ms) | 503 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 2814.102 | 0.370 | 0.424 | 1.998 | 2705 |
| 2 | 2853.471 | 0.244 | 0.298 | 1.431 | 1682 |
| 4 | 2880.291 | 0.151 | 0.212 | 0.809 | 1177 |
| 8 | 2763.494 | 0.535 | 0.736 | 3.588 | 3082 |

해석:

- `mixed` 부하에서는 `workers=4` 가 가장 좋은 결과를 보였다.
- `worker=1` 대비 `workers=4` 는 throughput 이 약 `2.3%` 높고, p95 는 약 `59.5%` 낮고, `503` 은 약 `56.5%` 적다.
- `workers=8` 은 오히려 성능이 나빠졌다.
- 즉 현재 구현에서는 worker 수를 무조건 크게 잡는 것이 아니라, 일정 지점 이후에는 contention 과 scheduling overhead 가 이득을 깎는 것으로 보인다.

## 5. 추가 관찰

`mixed @ 5000 rps` 도 별도로 두 번 측정했지만, worker 수별 우열이 안정적으로 재현되지 않았다.

- 한 번은 `worker=1` 이 가장 높게 나왔고
- 다른 한 번은 `worker=2` 가 가장 높게 나왔다

이 구간은 이미 서버가 포화에 가까워서 실험 노이즈가 커진 상태로 판단했다.  
따라서 결론은 포화 직전보다 한 단계 아래인 `mixed @ 3000 rps` 결과를 기준으로 삼는 것이 더 적절하다.

## 6. 결론

현재 프로젝트 기준 결론은 아래와 같다.

1. `read-heavy` 에서는 `worker=1` 과 multi-worker 의 처리량 차이가 크지 않다.
2. `mixed` 에서는 `worker=4` 가 가장 좋은 균형점을 보였다.
3. `worker=8` 은 현재 코드베이스에서 오히려 latency 와 `503` 을 악화시킬 수 있다.
4. 따라서 기본 worker 수는 "많을수록 좋다"가 아니라, 현재 구현에서는 `4` 근처가 더 합리적인 후보로 보인다.

## 7. 원본 결과 경로

대표 비교에 사용한 결과 디렉터리는 아래와 같다.

- `output/worker-comparison/read_heavy/`
- `output/worker-comparison/mixed-r3000d20/`

참고용 추가 실험 경로는 아래와 같다.

- `output/worker-comparison/pilot/`
- `output/worker-comparison/mixed/`
- `output/worker-comparison/mixed-r3/`

## 8. 인위적 Worker Work 추가 실험

### 8.1 왜 추가 실험이 필요한가

기본 `read_heavy` 경로는 `WHERE id = ?` 에 대한 B+Tree 조회라서 실행 시간이 매우 짧다.  
그래서 `worker=1` 이어도 이미 충분히 빨라서 multi-worker 이점이 거의 보이지 않았다.

worker 수 차이를 더 분명하게 보기 위해, 요청 하나당 worker가 아주 짧은 CPU 작업을 추가로 수행하도록 서버 옵션을 확장했다.

추가한 옵션은 아래와 같다.

- 서버 CLI: `--simulate-work-us N`
- 실험 스크립트 env: `SIMULATED_WORK_US=N`

이 옵션은 `/query` 처리 중 `engine_execute_sql()` 직전에 지정한 마이크로초 동안 busy-wait 형태의 CPU 작업을 수행한다.

### 8.2 sleep 대신 CPU work를 쓴 이유

짧은 `sleep` 도 worker 체류 시간을 늘리는 효과는 있다.  
하지만 그 경우는 CPU 계산량이 아니라 "대기 시간"을 늘리는 실험이 된다.

이번 비교의 목적은 "worker가 실제로 더 오래 일하게 만들면 multi-worker 이점이 얼마나 커지는가"를 보는 것이므로, `sleep` 대신 CPU busy-wait를 사용했다.

### 8.3 pilot 결과

`read_heavy @ 3000 rps, 10s, worker=1` 에서 먼저 pilot을 돌렸다.

| simulated work | throughput (rps) | avg execution (ms) | avg response (ms) | 503 | worker busy ratio |
| --- | ---: | ---: | ---: | ---: | ---: |
| `200us` | 2847.249 | 0.207 | 0.436 | 125 | 0.6716 |
| `500us` | 1816.950 | 0.508 | 16.981 | 10940 | 0.9465 |

해석:

- `200us` 는 아직 single-worker 가 크게 무너지지 않았다.
- `500us` 부터는 `worker=1` 이 사실상 포화돼 worker 수 차이가 분명히 드러날 조건이 됐다.

따라서 본 비교는 `simulated_work_us=500` 으로 진행했다.

### 8.4 본 실험 결과

조건: `read_heavy @ 3000 rps, 10s, simulated_work_us=500`

| workers | throughput (rps) | avg queue wait (ms) | avg execution (ms) | avg response (ms) | p95 success (ms) | 503 | worker busy ratio |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 1816.162 | 16.494 | 0.512 | 17.017 | 18.048 | 10969 | 0.9500 |
| 2 | 2865.493 | 0.216 | 0.505 | 0.745 | 1.861 | 83 | 0.7574 |
| 4 | 2857.416 | 0.079 | 0.506 | 0.612 | 1.014 | 107 | 0.3807 |
| 8 | 2854.494 | 0.050 | 0.506 | 0.583 | 0.989 | 117 | 0.1901 |

해석:

- `worker=1` 은 명확히 포화됐다.
- `worker=2` 로만 올려도 throughput 이 `1816 -> 2865 rps` 로 크게 증가했고, `503` 도 거의 사라졌다.
- `worker=4`, `worker=8` 은 처리량을 더 올리기보다는 queue wait 과 tail latency를 조금 더 안정화하는 쪽으로 기여했다.
- 즉 worker 수 확장 효과가 안 보였던 이유는 "멀티 worker가 쓸모없어서"가 아니라, 기존 `read_heavy` 작업이 너무 가벼워 single-worker 로도 충분했기 때문이다.

### 8.5 결론

현재 코드베이스에서 worker scaling을 확인하려면, 요청당 실제 service time이 충분히 길어야 한다.

- 너무 짧은 작업: `worker=1` 과 multi-worker 차이가 거의 안 보인다
- 적당히 무거운 작업: multi-worker 이점이 분명히 드러난다
- 이번 실험 기준으로는 `500us` 정도의 추가 CPU work가 차이를 보여주기에 충분했다

## 9. 추가 결과 경로

- `output/worker-comparison/heavy-pilot-200/`
- `output/worker-comparison/heavy-pilot-500/`
- `output/worker-comparison/heavy-500/`
