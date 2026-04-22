# Bounded Queue Size 결정 실험 계획

## 1. 실험 개요

이 실험의 목적은 현재 프로젝트의 C 기반 멀티스레드 SQL gateway 서버에서 `bounded queue size`를 몇으로 두는 것이 가장 합리적인지 결정하는 것이다.

이번 단계에서 검증할 핵심 질문은 아래 4개다.

1. queue size가 너무 작으면 burst를 얼마나 빨리 놓치는가
2. queue size가 너무 크면 얼마나 오래 기다리게 만들어서 "늦게 실패하는 서버"가 되는가
3. 현재 worker 수와 현재 DB 엔진 workload에서 burst 흡수와 latency 균형이 가장 좋은 지점은 어디인가
4. 지금 코드베이스의 기본값으로 무엇을 채택할 것인가

이 문서는 현재 프로젝트 상태를 반영해 작성했다.

- 서버 엔드포인트는 현재 `POST /query` 하나다
- 요청 형식은 `{ "sql": "..." }` 이다
- 서버 기본 worker 수는 현재 `src/server.c`의 `SERVER_WORKER_COUNT = 8` 이다
- queue capacity는 현재 `SERVER_QUEUE_CAPACITY = 32` 로 고정돼 있다
- 엔진은 메모리 기반 테이블 런타임을 사용하며, 테이블은 첫 `INSERT` 시 생성된다
- 테이블 동시성 제어는 현재 `src/table_runtime.c`의 테이블 단위 `pthread_rwlock_t` 기반이다

이번 실험은 `bounded queue size`의 영향만 보기 위해 아래 요소는 핵심 범위에서 제외한다.

- rate limiting
- retry / backoff / jitter
- priority queue
- request priority
- autoscaling

## 2. 가설

### 2.1 queue size가 작을 때의 가설

- `503 Service Unavailable` 이 빠르게 증가한다
- `queue full` 이벤트가 짧은 burst에서도 자주 발생한다
- 평균 latency와 p95 자체는 낮아 보일 수 있다
- 하지만 성공률과 실제 처리 완료 수가 너무 낮아서 burst 흡수 능력이 부족하다고 판단될 가능성이 높다

### 2.2 queue size가 클 때의 가설

- `503` 은 늦게 발생하거나 덜 발생한다
- 대신 `queue_wait_time` 이 길어져 p95, p99 latency가 급격히 커진다
- `execution_time` 자체는 크게 안 변하는데 `total_response_time` 만 커지는 패턴이 나타난다
- 즉 서버가 "거절을 늦게 하는 대신 오래 기다리게 하는" 형태로 바뀔 수 있다

### 2.3 적절한 queue size의 가설

- 짧은 burst는 흡수한다
- `503` 과 `queue full` 이 완전히 0이 아니어도 폭발적이지 않다
- p95, p99 latency가 급격히 무너지지 않는다
- `queue_wait_time` 이 `execution_time` 보다 지나치게 커지지 않는다
- 성공률과 tail latency의 trade-off가 가장 균형적이다

## 3. 고정 변수 / 변경 변수 / 측정 변수

### 3.1 통제 변수

이번 실험에서 고정할 값은 아래와 같다.

| 항목 | 고정값 | 이유 |
| --- | --- | --- |
| worker thread 수 | `8` | 현재 코드 기본값이며, 기존 설계 문서도 8을 기준으로 잡고 있다. queue만 비교하려면 worker 수는 고정해야 한다. |
| DB 데이터셋 크기 | 초기 `bench_users` 50,000 rows | 인덱스 조회가 충분히 의미 있고, 로컬 실험에서 시드 시간이 과도하지 않은 수준이다. |
| 서버 실행 환경 | 같은 머신, 같은 빌드 옵션, 같은 포트, 같은 백그라운드 상태 | 환경 차이가 queue size 효과를 덮지 않게 하기 위해서다. |
| SQL 종류 비율 | workload A, B별로 고정 | queue size 외 변수를 줄이기 위해서다. |
| 요청 payload 형식 | `POST /query`, JSON body `{ "sql": "..." }` | 현재 서버 API와 동일해야 한다. |
| 락 정책 | 현재 코드의 테이블 단위 `pthread_rwlock_t` 유지 | 실험 도중 락 정책을 바꾸면 queue size 효과를 분리할 수 없다. |
| rate limiting | 비활성화 | 이번 실험의 핵심이 아니다. |
| retry | 없음 | 재시도는 queue full 패턴을 왜곡한다. |
| 응답 body 크기 | 가능한 작은 결과셋 유지 | 대규모 응답 전송 시간이 queue 효과를 가리지 않게 하기 위해서다. |

### 3.2 독립 변수

이번 실험에서 바꿀 유일한 핵심 변수는 아래 하나다.

| 항목 | 값 후보 |
| --- | --- |
| bounded queue size | `8`, `32`, `64`, `128` |

선정 이유는 아래와 같다.

- `8`: worker 수와 동일한 최소 후보
- `32`: 현재 기본값이자 worker의 4배
- `64`: worker의 8배
- `128`: 그보다 큰 값 하나로, 큰 queue가 tail latency를 얼마나 악화시키는지 보기 위한 상한 후보

2차 실험에서만 arrival rate를 바꾼다.

| 항목 | 사용 시점 | 이유 |
| --- | --- | --- |
| arrival rate | breaking point 탐색 실험 | queue size를 고른 뒤, 그 값이 어디서 무너지기 시작하는지 보기 위해서다 |
| workload mix | read-heavy vs mixed | queue size 후보가 workload 변화에도 납득 가능한지 보기 위해서다 |

### 3.3 종속 변수

반드시 수집할 항목은 아래와 같다.

| 분류 | 지표 |
| --- | --- |
| latency | 평균 latency, p95 latency, p99 latency |
| 성공/실패 | 성공률, 실패율, 503 개수 |
| queue | queue full 발생 횟수, 평균 queue length, 최대 queue length |
| 처리량 | 테스트 시간 동안 throughput, 실제 처리 완료 요청 수 |
| 내부 시간 | 평균 queue wait time, 평균 execution time, 평균 total response time |

가능하면 추가 수집할 항목은 아래와 같다.

- worker busy ratio
- lock wait time
- `SELECT` / `INSERT` 별 latency 분리

중요한 비교 규칙은 아래와 같다.

- latency는 반드시 `성공 요청(HTTP 200)` 기준과 `전체 요청` 기준을 분리해서 본다
- 이유: `503` 은 매우 빠르게 실패하므로 전체 latency만 보면 오히려 "좋아 보이는 착시"가 생길 수 있다

## 4. 서버 계측 설계

### 4.1 이번 프로젝트에서 먼저 필요한 최소 코드 변경

현재 코드에서는 `worker count` 와 `queue capacity` 가 `src/server.c` 매크로로 고정돼 있으므로, 실험을 위해 먼저 아래 변경이 필요하다.

1. `SERVER_QUEUE_CAPACITY` 를 런타임 설정값으로 바꾼다
2. 가능하면 `SERVER_WORKER_COUNT` 도 런타임 설정값으로 바꾼다
3. `ServerRequestQueue.items` 를 동적 할당 배열로 바꾼다
4. `ServerRequest` 에 요청 도착 시각을 저장한다
5. `ServerMetrics` 구조체를 추가한다

권장 구조는 아래와 같다.

```c
typedef struct {
    int worker_count;
    int queue_capacity;
    int listen_backlog;
} ServerConfig;

typedef struct {
    int client_fd;
    struct timespec request_arrival_time;
} ServerRequest;

typedef struct {
    unsigned long long total_requests_received;
    unsigned long long total_requests_completed;
    unsigned long long total_requests_failed;
    unsigned long long total_503_responses;
    unsigned long long total_queue_full_events;
    unsigned long long enqueue_success_count;
    unsigned long long enqueue_fail_count;

    unsigned long long queue_wait_ns_total;
    unsigned long long execution_ns_total;
    unsigned long long total_response_ns_total;

    unsigned long long current_queue_length;
    unsigned long long max_queue_length_observed;

    unsigned long long queue_length_time_ns_acc;
    struct timespec queue_length_last_changed_at;
} ServerMetrics;
```

시간 측정은 반드시 `clock_gettime(CLOCK_MONOTONIC, ...)` 기반으로 한다.
`gettimeofday()` 는 wall clock 변화 영향을 받으므로 이번 목적에 적합하지 않다.

### 4.2 필수 카운터

반드시 넣을 카운터는 아래와 같다.

| 카운터 | 의미 | 권장 삽입 위치 |
| --- | --- | --- |
| `total_requests_received` | accept 후 queue 진입 시도 수 | `server_queue_push()` 진입 직전 또는 내부 |
| `total_requests_completed` | 응답 전송까지 끝난 요청 수 | `server_send_response()` 성공 이후 |
| `total_requests_failed` | 4xx/5xx 포함 실패 응답 수 | `server_send_json_error()` 또는 status 분기 후 |
| `total_503_responses` | 실제 503 응답 수 | queue full 분기와 기타 503 사용 지점 |
| `total_queue_full_events` | queue full 감지 횟수 | `server_queue_push()` 에서 capacity 초과 시 |
| `enqueue_success_count` | queue push 성공 횟수 | `server_queue_push()` 성공 시 |
| `enqueue_fail_count` | queue push 실패 횟수 | `server_queue_push()` 실패 시 |

### 4.3 queue 관련 계측

반드시 수집할 queue 상태는 아래와 같다.

| 지표 | 계산 방식 | 권장 위치 |
| --- | --- | --- |
| `current_queue_length` | 현재 `queue->count` | queue mutex 안 |
| `max_queue_length_observed` | `max(max, queue->count)` | enqueue 성공 직후 |
| `average_queue_length_observed` | 시간가중 평균 queue length | enqueue/dequeue 시 queue 길이 변경마다 갱신 |

`average_queue_length_observed` 는 단순 샘플 평균보다 시간가중 평균을 권장한다.

계산 방식:

1. queue 길이가 바뀌기 직전 현재 길이와 경과 시간을 누적한다
2. `queue_length_time_ns_acc += current_queue_length * elapsed_ns`
3. 실험 종료 시 `average_queue_length = queue_length_time_ns_acc / total_observation_time_ns`

이 방식이 필요한 이유는 queue가 잠깐 128까지 튄 경우와, 32 수준에서 오래 머문 경우를 구분할 수 있기 때문이다.

### 4.4 latency 타이머

각 요청마다 아래 시각을 기록해야 한다.

| 시각 | 의미 | 권장 위치 |
| --- | --- | --- |
| `request_arrival_time` | accept 후 queue 진입 시점 | `server_queue_push()` 호출 직전 |
| `dequeue_time` | worker가 queue에서 꺼낸 시점 | `server_queue_pop()` 성공 직후 |
| `execution_start_time` | SQL 실행 시작 시점 | `engine_execute_sql()` 호출 직전 |
| `execution_end_time` | SQL 실행 종료 시점 | `engine_execute_sql()` 반환 직후 |
| `response_send_end_time` | 응답 전송 완료 시점 | `server_send_response()` 성공 직후 |

최소 계산 항목은 아래와 같다.

| 계산값 | 공식 |
| --- | --- |
| `queue_wait_time` | `dequeue_time - request_arrival_time` |
| `execution_time` | `execution_end_time - execution_start_time` |
| `total_response_time` | `response_send_end_time - request_arrival_time` |

### 4.5 함수 단위 삽입 지점

현재 코드 기준 권장 삽입 지점은 아래와 같다.

| 파일 / 함수 | 넣을 계측 |
| --- | --- |
| `src/server.c:server_queue_push()` | received 카운트, enqueue 성공/실패, queue full, current/max/avg queue length |
| `src/server.c:server_queue_pop()` | dequeue timestamp, current/avg queue length |
| `src/server.c:server_handle_client()` | request별 queue wait, execution, total response time |
| `src/server.c:server_send_response()` | 응답 종료 시각, completed 카운트 |
| `src/server.c:server_send_json_error()` | 실패 카운트 |
| `src/server.c:server_run()` | 테스트 시작/종료 시각, summary 출력 |
| `src/table_runtime.c:table_runtime_acquire_locked_entry()` | 선택 사항: lock wait time 측정 |

### 4.6 로그 / 메트릭 노출 방식 추천

이번 프로젝트의 1차 실험에서는 `/metrics` endpoint보다 `종료 시 stdout summary 출력`을 추천한다.

이유는 아래와 같다.

1. 현재 서버는 `POST /query` 단일 라우트 중심이라 `/metrics` 를 추가하려면 라우팅 분기와 직렬화 코드를 더 건드려야 한다
2. `/metrics` 요청 자체가 같은 worker/queue를 사용하면 측정값을 오염시킬 수 있다
3. 종료 시 summary는 구현이 가장 단순하고, 각 실험 run의 로그 파일을 그대로 artifact로 남기기 쉽다

추천 방식은 아래 2단계다.

1. 필수: 서버 종료 시 최종 summary 1회 출력
2. 선택: 5초 간격의 delta summary 출력

권장 summary 항목:

- 총 수신 요청 수
- 총 완료 요청 수
- 총 실패 요청 수
- 총 503 수
- queue full 횟수
- 평균 / 최대 queue 길이
- 평균 queue wait time
- 평균 execution time
- 평균 total response time
- 처리량

## 5. k6 실험 시나리오

### 5.1 왜 arrival-rate 기반 executor를 쓰는가

이번 실험은 queue가 얼마나 요청 burst를 흡수하는지 보는 것이 목적이다.
따라서 `closed model` 보다 `arrival-rate 기반` 이 적합하다.

이유는 아래와 같다.

- closed model은 서버가 느려질수록 클라이언트가 다음 요청을 덜 보내므로 overload가 완화되는 방향으로 왜곡된다
- bounded queue 실험은 "얼마나 빠르게 요청이 들어오느냐" 가 핵심이므로, 도착률을 고정하는 편이 queue pressure를 더 직접적으로 관찰할 수 있다
- `503` 과 queue full 패턴도 arrival-rate 기반에서 더 명확하게 드러난다

권장 executor:

- 실험 1: `constant-arrival-rate`
- 실험 2: `ramping-arrival-rate`

### 5.2 workload 설계

#### workload A: read-heavy

- 비율: `100% SELECT`
- SQL 예시:

```json
{ "sql": "SELECT * FROM bench_users WHERE id = 12345;" }
```

설계 이유:

- 인덱스를 타는 빠른 read path를 최대한 순수하게 본다
- write lock 영향 없이 queue size와 worker saturation을 먼저 본다
- "queue가 너무 작아서 burst를 놓치는지" 를 가장 깨끗하게 관찰할 수 있다

#### workload B: mixed

- 비율: `80% SELECT / 20% INSERT`
- SQL 예시:

```json
{ "sql": "SELECT * FROM bench_users WHERE id = 12345;" }
{ "sql": "INSERT INTO bench_users (name, age) VALUES ('user_90001', 29);" }
```

설계 이유:

- `INSERT` 가 실제 row와 B+Tree를 수정하므로 service time이 늘어나고 write lock 경쟁도 생긴다
- 같은 queue size라도 read-heavy 때와 mixed 때 tail latency와 503 패턴이 어떻게 바뀌는지 볼 수 있다
- queue가 너무 크면 mixed workload에서 더 심한 "늦게 실패" 양상이 나타날 가능성이 높다

중요한 통제 규칙:

- `SELECT` 는 항상 `WHERE id = ?` 형태의 인덱스 조회만 사용한다
- `SELECT` 대상 id 범위는 초기 시드 데이터 `1..50000` 에 고정한다
- `INSERT` 는 같은 `bench_users` 테이블에 넣는다
- 이렇게 해야 read path 자체는 안정적으로 유지하면서, write가 queue와 락에 주는 영향만 비교할 수 있다

### 5.3 데이터셋 준비

현재 서버는 메모리 기반이므로 각 실험 run마다 서버를 fresh start 한 뒤 동일한 데이터셋을 다시 시드해야 한다.

권장 데이터셋:

| 항목 | 값 |
| --- | --- |
| 테이블명 | `bench_users` |
| 초기 row 수 | `50,000` |
| 컬럼 | `id`, `name`, `age` |
| 인덱스 | `id` B+Tree 인덱스 사용 |

시드 방식:

1. 서버 시작
2. 별도 seeding 스크립트로 `INSERT INTO bench_users (name, age) VALUES (...)` 를 50,000회 전송
3. 시드 완료 후 30초 idle 또는 짧은 warm-up 수행
4. 그 다음 k6 부하 테스트 시작

현재 프로젝트에서는 CLI로 SQL 파일을 실행해도 서버 프로세스와 메모리를 공유하지 않으므로, 시드는 반드시 서버 프로세스에 HTTP로 넣어야 한다.

### 5.4 파일 구조 제안

`k6` 스크립트는 아래 구조를 권장한다.

```text
loadtest/
├── k6/
│   ├── common.js
│   ├── payloads.js
│   ├── read_heavy.js
│   ├── mixed.js
│   ├── queue_sweep.js
│   └── breaking_point.js
└── seed/
    └── seed_bench_users.js
```

역할은 아래와 같다.

| 파일 | 역할 |
| --- | --- |
| `common.js` | 공통 request 전송, status 분류, env 파싱 |
| `payloads.js` | SELECT / INSERT payload 생성 |
| `read_heavy.js` | workload A 정의 |
| `mixed.js` | workload B 정의 |
| `queue_sweep.js` | 실험 1 orchestration |
| `breaking_point.js` | 실험 2 orchestration |
| `seed_bench_users.js` | 실험 시작 전 데이터 시드 |

### 5.5 환경 변수

최소 환경 변수는 아래와 같이 둔다.

| 변수 | 예시 | 용도 |
| --- | --- | --- |
| `BASE_URL` | `http://127.0.0.1:8080` | 서버 주소 |
| `ARRIVAL_RATE` | `400` | 초당 요청 도착률 |
| `WORKLOAD_TYPE` | `read-heavy` / `mixed` | workload 선택 |
| `TEST_DURATION` | `60s` | 본 실험 시간 |
| `PRE_ALLOCATED_VUS` | `200` | arrival-rate executor 보조 |
| `MAX_VUS` | `1000` | burst 대응 upper bound |
| `SEED_ROWS` | `50000` | 초기 데이터 수 |
| `SELECT_ID_MAX` | `50000` | read 대상 최대 id |

`QUEUE_SIZE` 는 `k6` 환경 변수가 아니라 서버 시작 파라미터로 바꾸는 것이 맞다.
권장 예시는 `./sql_processor --server 8080 --workers 8 --queue 32` 형태다.

### 5.6 k6 샘플 스크립트 구조

#### `read_heavy.js`

```javascript
import http from 'k6/http';
import { check } from 'k6';
import { Counter, Trend } from 'k6/metrics';
import { randomIntBetween } from 'https://jslib.k6.io/k6-utils/1.4.0/index.js';

const status503 = new Counter('status_503');
const successDuration = new Trend('success_duration');

export const options = {
  scenarios: {
    read_heavy: {
      executor: 'constant-arrival-rate',
      rate: Number(__ENV.ARRIVAL_RATE),
      timeUnit: '1s',
      duration: __ENV.TEST_DURATION || '60s',
      preAllocatedVUs: Number(__ENV.PRE_ALLOCATED_VUS || 200),
      maxVUs: Number(__ENV.MAX_VUS || 1000),
    },
  },
  thresholds: {
    http_req_failed: ['rate<0.05'],
    success_duration: ['p(95)<200'],
  },
};

export default function () {
  const id = randomIntBetween(1, Number(__ENV.SELECT_ID_MAX || 50000));
  const payload = JSON.stringify({
    sql: `SELECT * FROM bench_users WHERE id = ${id};`,
  });

  const res = http.post(`${__ENV.BASE_URL}/query`, payload, {
    headers: { 'Content-Type': 'application/json' },
    tags: { workload: 'read-heavy' },
  });

  if (res.status === 503) status503.add(1);
  if (res.status === 200) successDuration.add(res.timings.duration);
  check(res, { 'status is expected': (r) => [200, 503].includes(r.status) });
}
```

#### `mixed.js`

```javascript
import http from 'k6/http';
import { randomIntBetween } from 'https://jslib.k6.io/k6-utils/1.4.0/index.js';

export const options = {
  scenarios: {
    mixed: {
      executor: 'constant-arrival-rate',
      rate: Number(__ENV.ARRIVAL_RATE),
      timeUnit: '1s',
      duration: __ENV.TEST_DURATION || '60s',
      preAllocatedVUs: Number(__ENV.PRE_ALLOCATED_VUS || 200),
      maxVUs: Number(__ENV.MAX_VUS || 1000),
    },
  },
};

export default function () {
  const doInsert = Math.random() < 0.2;
  const sql = doInsert
    ? `INSERT INTO bench_users (name, age) VALUES ('user_${__VU}_${__ITER}', ${randomIntBetween(20, 60)});`
    : `SELECT * FROM bench_users WHERE id = ${randomIntBetween(1, Number(__ENV.SELECT_ID_MAX || 50000))};`;

  http.post(`${__ENV.BASE_URL}/query`, JSON.stringify({ sql }), {
    headers: { 'Content-Type': 'application/json' },
    tags: { workload: 'mixed', sql_type: doInsert ? 'insert' : 'select' },
  });
}
```

threshold 예시는 비교용 guardrail로만 쓴다.
절대 통과 기준으로 쓰지 말고, run 간 상대 비교 기준으로 써야 한다.

### 5.7 실험 1: queue size sweep

목표:

- queue size 자체의 영향만 본다

방법:

1. worker 수는 `8`로 고정
2. workload별로 arrival rate를 하나 고정
3. queue size만 `8`, `32`, `64`, `128` 로 바꿔 반복
4. 각 조건을 최소 3회 반복

권장 duration:

- warm-up: `15s`
- measurement: `60s`
- cool-down 또는 summary 수집: `10s`

고정 arrival rate 선정 방식:

현재 서버 성능이 아직 확정되지 않았으므로, 본 실험 전에 아주 짧은 calibration 1회를 수행한다.

1. `queue=32`, workload A로 시작
2. arrival rate를 `100 -> 200 -> 300 -> 400 ...` 식으로 30초씩 증가
3. `503` 이 거의 없고 p95가 급상승하지 않는 마지막 rate를 `R_stable` 로 정의
4. queue sweep에는 `R_sweep = ceil(R_stable * 1.15)` 를 사용

이 calibration은 비교 실험의 일부가 아니라, "queue size 차이가 보이도록 적절한 압력"을 고르는 준비 단계다.

### 5.8 실험 2: breaking point 찾기

목표:

- 선정된 queue size 후보가 어느 arrival rate에서 급격히 나빠지는지 찾는다

방법:

1. 실험 1에서 가장 유망한 queue size 하나를 고정
2. `ramping-arrival-rate` 로 arrival rate를 단계적으로 올린다
3. p95/p99, queue wait, 503, throughput plateau를 관찰한다

권장 단계 예시:

| 단계 | arrival rate |
| --- | --- |
| 1 | `0.8 x R_stable` |
| 2 | `1.0 x R_stable` |
| 3 | `1.2 x R_stable` |
| 4 | `1.4 x R_stable` |
| 5 | `1.6 x R_stable` |

각 단계는 30초 이상 유지한다.

### 5.9 k6에서 최소 수집할 지표

| 지표 | 목적 |
| --- | --- |
| `http_req_duration` | 외부 관점 latency |
| `http_req_failed` | 실패율 |
| status code 분포 | 200/400/404/503 비율 구분 |
| `iteration_rate` | 부하 생성 안정성 확인 |
| request rate | 목표 arrival rate가 실제로 유지됐는지 확인 |

해석 규칙:

- 정상 overload 실험에서는 status code가 사실상 `200` 또는 `503` 로만 나와야 한다
- `400`, `404`, `405` 가 보이면 queue 문제라기보다 테스트 payload 또는 서버 라우팅 버그로 간주하고 해당 run을 무효 처리한다

가능하면 아래도 같이 수집한다.

- 서버 종료 summary 로그
- run별 `server_summary.json` 파일

권장 방식은 k6가 `/metrics` 를 polling하지 않고, 각 run 종료 후 서버 summary를 artifact로 저장하는 것이다.

## 6. 실행 절차

### 6.1 사전 준비

체크리스트:

- 서버 빌드 옵션 고정
- 실험 중 다른 CPU 집약 프로세스 종료
- 서버 로그 저장 경로 준비
- k6 결과 저장 경로 준비
- 시드 스크립트 준비
- 서버가 `--queue`, `--workers` 파라미터를 받을 수 있게 수정
- summary 출력 또는 summary 파일 저장 구현

### 6.2 step by step

#### 0단계. calibration

1. 서버를 `worker=8`, `queue=32` 로 실행한다
2. `bench_users` 50,000 rows를 시드한다
3. workload A로 30초짜리 짧은 ramp test를 실행한다
4. `R_stable` 을 기록한다
5. `R_sweep = ceil(R_stable * 1.15)` 를 정한다

#### 1단계. 실험 1, workload A queue sweep

1. 서버를 `worker=8`, `queue=8` 로 실행한다
2. 동일한 시드 데이터 50,000 rows를 넣는다
3. warm-up 15초를 수행한다
4. workload A를 `ARRIVAL_RATE=R_sweep`, `TEST_DURATION=60s` 로 실행한다
5. k6 결과와 서버 summary를 저장한다
6. 같은 조건으로 총 3회 반복한다
7. queue size를 `32`, `64`, `128` 으로 바꿔 동일하게 반복한다

#### 2단계. 실험 1, workload B queue sweep

1. workload만 B로 바꾸고 나머지는 동일하게 반복한다
2. 각 queue size마다 최소 3회 측정한다
3. 중앙값 기준으로 비교한다

#### 3단계. 유망 후보 선정

1. workload A와 B 결과를 나란히 놓고 비교한다
2. 가장 작은 p95만 보지 말고, 성공률과 queue wait을 함께 본다
3. 후보를 1개, 많아도 2개만 남긴다

#### 4단계. 실험 2, breaking point 탐색

1. 최종 후보 queue size를 고정한다
2. workload A로 arrival rate ramp test를 수행한다
3. workload B로도 동일하게 수행한다
4. throughput plateau, p95 급상승, 503 증가 지점을 기록한다
5. 기본값 채택 여부를 결정한다

### 6.3 반복 규칙

- 모든 조건은 최소 3회 반복한다
- 비교 시 평균보다 중앙값을 우선 본다
- 단 3회 값의 분산도 함께 기록한다
- 1회 결과만 보고 결론을 내리지 않는다

## 7. 결과 정리 표 양식

### 7.1 run 단위 결과표

| queue size | workload type | arrival rate | run | avg latency(ms) | p95(ms) | p99(ms) | success rate | failure rate | 503 count | queue full count | throughput(req/s) | avg queue length | max queue length | avg queue wait(ms) | avg execution(ms) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 8 | read-heavy | 400 | 1 |  |  |  |  |  |  |  |  |  |  |  |  |

### 7.2 집계표

| queue size | workload type | arrival rate | median p95(ms) | median p99(ms) | median success rate | median 503 count | median avg queue wait(ms) | median throughput(req/s) | 해석 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 8 | read-heavy | 400 |  |  |  |  |  |  |  |

### 7.3 breaking point 표

| selected queue size | workload type | arrival rate 단계 | median p95(ms) | median 503 count | median queue wait(ms) | median throughput(req/s) | 관찰 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 32 | mixed | 1.4x R_stable |  |  |  |  |  |

## 8. 판단 기준

### 8.1 queue size가 너무 작은 경우의 시그널

- `503` 이 너무 빨리 증가한다
- `queue full` 이 짧은 burst에도 빈번하다
- latency는 낮아 보일 수 있다
- 하지만 성공률이 낮고 완료 요청 수가 부족하다
- throughput이 worker capacity보다 빨리 꺾인다

해석:

- fail-fast는 잘 되지만 burst 흡수 능력이 부족하다
- 운영 기본값으로 쓰기에는 너무 공격적으로 reject 하는 설정이다

### 8.2 queue size가 너무 큰 경우의 시그널

- `503` 은 늦게 나타난다
- p95, p99 latency가 급격히 커진다
- `queue_wait_time` 이 `execution_time` 보다 훨씬 커진다
- 성공 요청 중에서도 매우 느린 요청이 많아진다
- throughput은 거의 안 늘었는데 응답 시간만 길어진다

해석:

- 서버가 overload를 숨기고 대기열로 떠넘기고 있다
- "성공은 하지만 너무 늦은" 요청이 많아지는 전형적인 늦게 실패하는 서버다

### 8.3 적절한 queue size의 시그널

- 짧은 burst에서 503이 너무 빨리 폭증하지 않는다
- p95, p99가 큰 폭으로 무너지지 않는다
- `avg queue wait` 이 증가하더라도 `avg execution time` 과 비교해 납득 가능한 수준이다
- throughput 증가가 실제로 관찰된다
- mixed workload에서도 read-heavy 대비 열화가 과도하지 않다

### 8.4 최종 판단 규칙

queue size는 "실패가 가장 적은 값" 으로 고르지 않는다.

아래 4개를 함께 만족하는 값을 우선한다.

1. burst 흡수 효과가 있다
2. p95, p99 latency가 과도하게 상승하지 않는다
3. queue wait이 execution보다 지나치게 커지지 않는다
4. mixed workload에서도 설명 가능한 패턴을 보인다

## 9. 추천 기본값 선정 방식

### 9.1 선택 로직

실험 후 기본 queue size는 아래 순서로 결정한다.

1. workload A와 B 모두에서 탈락 후보를 제거한다
2. 탈락 기준은 아래 중 하나라도 강하게 나타나는 경우다
3. 남은 후보 중 더 작은 queue를 우선한다

탈락 기준:

- 성공률은 높지만 p95/p99와 queue wait이 과도한 경우
- latency는 좋지만 503과 queue full이 너무 빨리 폭증하는 경우
- workload A와 B 결과 차이가 지나치게 커서 기본값으로 설명하기 어려운 경우

남은 후보가 2개면 아래 원칙을 적용한다.

- throughput 차이가 작으면 더 작은 queue를 선택한다
- throughput 차이가 유의미하게 크고 p95/p99 열화가 제한적이면 더 큰 queue를 선택한다

### 9.2 현재 프로젝트 기준 초기 추천

현재 코드와 기존 설계 문서 기준으로는 `queue size = 32` 를 가장 유력한 초기 후보로 둔다.

이유:

- 이미 현재 구현 기본값이다
- `worker=8` 기준 정확히 4배라서 burst 흡수와 fail-fast 사이의 균형점으로 자주 쓰는 시작점이다
- `8` 보다는 순간 burst에 덜 취약하고, `64` 나 `128` 보다는 tail latency 폭증 위험이 낮다

단, 이 값은 가설 수준의 시작점일 뿐이고 실험 결과가 아래처럼 나오면 바꿔야 한다.

- `32` 도 너무 빨리 503을 낸다면 `64` 검토
- `32` 에서 이미 queue wait이 execution보다 과도하게 길다면 `16` 또는 `8` 재검토

## 10. 추가 확장 아이디어

이번 실험이 끝난 뒤 후순위로 검토할 만한 항목은 아래와 같다.

1. worker 수까지 포함한 2차원 실험
2. `/metrics` endpoint 또는 JSON summary 파일 추가
3. rate limiting, retry, priority queue 같은 overload 정책 비교

주의:

- 위 항목들은 이번 실험의 핵심이 아니다
- 이번 단계에서는 반드시 `queue size` 중심 해석을 먼저 끝내야 한다

---

## 즉시 실행 요약

### 가장 먼저 실행할 최소 실험 버전

- `worker=8` 고정
- `queue size = 8, 32, 64, 128`
- workload A는 `100% SELECT`
- `queue=32` 로 짧은 calibration 후 `R_sweep` 결정
- 각 queue size마다 60초 측정, 3회 반복

### 오늘 바로 할 수 있는 가장 작은 실험

아직 계측이 없다면 가장 작은 버전은 아래다.

1. `src/server.c` 에 `queue full count`, `503 count`, `max queue length` 만 먼저 넣는다
2. `queue size` 를 런타임 인자로 바꾼다
3. workload A 하나만 사용한다
4. `queue=8` 과 `queue=32` 두 값만 먼저 비교한다
5. k6는 `constant-arrival-rate` 1개 시나리오만 쓴다

이 최소 버전만으로도 "queue가 너무 작은지" 는 바로 보이기 시작한다.

### 가장 추천하는 초기 queue size 후보

- `32`

### 실험 전 가설 수준의 추천값

- `worker=8` 기준 `queue size = 32` 를 1순위 가설로 추천한다

### 가장 위험한 해석 실수 3개

1. 성공률만 보고 queue를 너무 크게 잡는 실수
2. `503` 이 적다는 이유만으로 좋은 설정이라고 결론 내리는 실수
3. 1회 측정만 보고 queue size를 확정하는 실수

추가로 특히 조심할 점:

- 전체 latency만 보고 503의 빠른 실패를 "좋은 지연시간" 으로 오해하지 말 것
- rate limit이나 retry가 켜진 상태에서 queue 효과와 혼동하지 말 것
