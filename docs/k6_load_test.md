# k6 Load Test Guide

이 문서는 현재 `POST /query` 서버가 병렬 요청을 어떻게 처리하는지 확인하기 위한 `k6` 시나리오 모음이다.

## 준비

1. 서버를 fresh 상태로 띄운다.

```bash
make
./sql_processor --server 8080
```

2. 다른 터미널에서 `k6`를 실행한다.

```bash
k6 run loadtests/k6/baseline_fast_lookup.js
```

기본 주소는 `http://127.0.0.1:8080` 이다.

다른 주소를 쓰려면:

```bash
BASE_URL=http://127.0.0.1:18080 k6 run loadtests/k6/baseline_fast_lookup.js
```

## 공통 원칙

- 모든 스크립트는 `setup()`에서 테스트용 테이블을 새로 seed한다.
- 현재 DB는 메모리 기반이므로, 가장 깔끔한 방법은 **스크립트마다 서버를 재시작**하는 것이다.
- 느린 요청은 `id`가 아닌 컬럼에 대한 전체 스캔으로 만든다.
- 빠른 요청은 `WHERE id = ?` 인덱스 조회를 사용한다.

조절 가능한 주요 환경 변수:

- `BASE_URL`: 서버 주소
- `BIG_ROWS`: 큰 테이블 row 수, 기본 `5000`
- `SMALL_ROWS`: 작은 테이블 row 수, 기본 `100`
- `TEST_DURATION`: 일반 시나리오 길이, 기본 `20s`
- `FAST_VUS`: 빠른 조회 VU 수
- `SLOW_VUS`: 느린 스캔 VU 수
- `FAST_P95_MS`: 빠른 조회 목표 p95 ms
- `OVERLOAD_RATE`: overload 시도 요청 rate, 기본 `120`

차이가 잘 안 보이면:

- `BIG_ROWS`를 `10000`, `20000`까지 올린다.
- `SLOW_VUS`를 늘린다.
- overload 테스트에서는 `OVERLOAD_RATE`를 더 크게 올린다.

## 1. Baseline

파일:

- `loadtests/k6/baseline_fast_lookup.js`

목적:

- 인덱스 조회만 단독으로 돌렸을 때의 기준 p95 latency를 측정한다.

실행:

```bash
k6 run loadtests/k6/baseline_fast_lookup.js
```

해석:

- `fast_lookup_duration` 의 `p(95)`를 기록해 둔다.
- 이후 mixed 테스트에서 이 값과 비교한다.

## 2. Same-Table Read/Read

파일:

- `loadtests/k6/mixed_same_table_read_read.js`

목적:

- 같은 테이블에서 `slow scan` 과 `fast lookup` 이 동시에 돌아갈 때,
  빠른 조회가 얼마나 유지되는지 본다.

실행:

```bash
k6 run loadtests/k6/mixed_same_table_read_read.js
```

해석:

- `fast_lookup_duration p95` 가 baseline과 비슷하면 read/read 병렬성이 어느 정도 유지된다고 볼 수 있다.
- 이 값이 slow scan 시간만큼 크게 튀면 사실상 직렬 처리일 가능성이 높다.

## 3. Same-Table Read/Write

파일:

- `loadtests/k6/read_write_conflict.js`

목적:

- 같은 테이블에서 긴 `SELECT` 와 `INSERT` 가 겹칠 때 write가 얼마나 지연되는지 본다.

실행:

```bash
k6 run loadtests/k6/read_write_conflict.js
```

해석:

- `writer_duration` 이 baseline보다 커지면 write가 reader와 lock 경합 중이라는 뜻이다.
- 현재 설계상 이런 지연은 어느 정도 예상된 동작이다.

## 4. Cross-Table Isolation

파일:

- `loadtests/k6/cross_table_isolation.js`

목적:

- 큰 테이블에서 느린 스캔이 돌아가는 동안, 다른 테이블의 빠른 조회가 유지되는지 본다.

실행:

```bash
k6 run loadtests/k6/cross_table_isolation.js
```

해석:

- `isolated_lookup_duration p95` 가 baseline과 크게 다르지 않으면
  table-level lock 격리가 제대로 동작하는 쪽에 가깝다.

## 5. Overload / Queue Full

파일:

- `loadtests/k6/overload_queue.js`

목적:

- worker 8개와 queue 32개를 넘기는 상황에서,
  서버가 `503`으로 fail-fast 하는지 확인한다.

실행:

```bash
k6 run loadtests/k6/overload_queue.js
```

필요하면 더 강하게:

```bash
BIG_ROWS=20000 SLOW_VUS=8 OVERLOAD_RATE=300 \
k6 run loadtests/k6/overload_queue.js
```

해석:

- `queue_rejected` 가 0보다 크면 queue full 응답이 발생한 것이다.
- `queue_accepted` 와 함께 보면, 과부하 시 일부 요청을 받으면서 일부는 빠르게 거절하는지 볼 수 있다.

## 추천 순서

1. `baseline_fast_lookup.js`
2. `mixed_same_table_read_read.js`
3. `cross_table_isolation.js`
4. `read_write_conflict.js`
5. `overload_queue.js`

가장 중요한 비교값은 이 두 개다.

- baseline의 `fast_lookup_duration p95`
- mixed 상황의 `fast_lookup_duration p95`

이 둘의 차이가 작으면 “느린 조회가 있어도 빠른 조회는 같이 돈다”는 근거가 된다.
