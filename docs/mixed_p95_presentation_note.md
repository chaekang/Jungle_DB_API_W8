# Why `workers=8` Made `p95` Worse In Mixed Workload

## 발표용 한 줄 요약

`mixed` workload에서는 요청 일부가 `INSERT`라서 write lock 경쟁이 생기고, worker를 8개까지 늘리면 병렬성이 늘어나는 것보다 lock contention과 queue 대기가 더 커져서 p95가 오히려 악화됐다.

## 발표 멘트

이번 결과에서 조금 이상해 보이는 지점은 `workers=8`일 때입니다.  
보통 스레드가 많아지면 더 빨라질 것 같지만, 이 실험에서는 오히려 `p95`가 커졌습니다.

이유는 `mixed` workload가 read-only가 아니기 때문입니다.  
이 부하에는 `SELECT`뿐 아니라 `INSERT`도 섞여 있습니다.

현재 구현에서는 `SELECT`는 read lock을 잡고, `INSERT`는 같은 테이블에 write lock을 잡습니다.  
즉 worker가 많아질수록 독립적으로 빨라지는 게 아니라, 같은 테이블 락 하나를 두고 더 많은 스레드가 경쟁하게 됩니다.

그래서 `workers=4` 정도까지는 병렬 처리 이점이 있었지만, `workers=8`부터는 lock contention, context switching, queue 대기 비용이 더 커졌습니다.  
그 결과 평균뿐 아니라 tail latency인 `p95`가 특히 더 크게 악화됐습니다.

즉 이 결과는 "스레드가 많으면 무조건 좋다"가 아니라,  
"공유 자원 경쟁이 있는 workload에서는 적정 worker 수가 따로 있다"는 것을 보여줍니다.

## 발표에서 바로 보여줄 수 있는 데이터

조건: `mixed @ 3000 rps, 20s`

| workers | throughput (rps) | avg queue wait (ms) | avg response (ms) | p95 success (ms) | 503 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 4 | 2880.291 | 0.151 | 0.212 | 0.809 | 1177 |
| 8 | 2763.494 | 0.535 | 0.736 | 3.588 | 3082 |

발표 포인트는 아래처럼 잡으면 된다.

1. `throughput`도 떨어졌다: `2880 -> 2763 rps`
2. `queue wait`가 늘었다: `0.151 -> 0.535 ms`
3. `avg response`도 늘었다: `0.212 -> 0.736 ms`
4. `p95`는 더 크게 튀었다: `0.809 -> 3.588 ms`
5. `503`도 늘었다: `1177 -> 3082`

즉 `workers=8`은 일을 더 많이 처리한 것이 아니라, 더 많이 경쟁하면서 더 많이 밀린 상태로 볼 수 있다.

## 코드 근거

- `mixed` workload는 `SELECT`와 `INSERT`를 섞어 보낸다: [loadtest/k6/mixed.js](/Users/sisu/Projects/jungle/WEDCodingDay/Jungle_DB_API_Week8/loadtest/k6/mixed.js:28)
- `INSERT`는 write lock 경로를 탄다: [src/executor.c](/Users/sisu/Projects/jungle/WEDCodingDay/Jungle_DB_API_Week8/src/executor.c:330)
- `SELECT`는 read lock 경로를 탄다: [src/executor.c](/Users/sisu/Projects/jungle/WEDCodingDay/Jungle_DB_API_Week8/src/executor.c:368)
- 실제 락은 테이블 단위 `pthread_rwlock`이다: [src/table_runtime.c](/Users/sisu/Projects/jungle/WEDCodingDay/Jungle_DB_API_Week8/src/table_runtime.c:244)

## 발표용 마무리 멘트

정리하면, `workers=8`에서 p95가 더 나빠진 건 비정상 동작이라기보다 현재 구조에서 자연스러운 결과입니다.  
이 프로젝트에서는 worker 수를 무조건 늘리는 것보다, workload와 락 경쟁 구조에 맞는 sweet spot을 찾는 게 더 중요합니다.  
이번 mixed workload에서는 그 sweet spot이 `4 workers`였습니다.
