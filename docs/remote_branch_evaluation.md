# Remote Branch Evaluation

## 목적

이 문서는 [`docs/concurrent_server_refactor_evaluation_criteria.md`](./concurrent_server_refactor_evaluation_criteria.md)
기준으로 현재 remote branch 구현들을 평가한 결과를 정리한다.

평가 대상:

- `origin/eunyeol`
- `origin/feat/chaekang`
- `origin/hg`
- `origin/km`

기준 branch:

- `origin/main`

평가 시점:

- 2026-04-22

---

## 평가 방법

- 각 branch를 별도 임시 clone에서 checkout 해서 확인했다.
- 정적 코드 구조와 테스트 코드를 함께 읽었다.
- 가능하면 `make tests` 를 직접 실행해 동적 증거를 확보했다.
- 최종 판정은 평가 기준 문서의 `Gate` 항목 기준으로 내렸다.

테스트 실행 결과:

- `origin/eunyeol`: `16 passed, 0 failed`
- `origin/feat/chaekang`: `19 passed, 1 failed`
- `origin/hg`: `16 passed, 0 failed`
- `origin/km`: `16 passed, 0 failed`

---

## 최종 요약

| Branch | 최종 판정 | 한 줄 요약 |
| --- | --- | --- |
| `origin/eunyeol` | `계획 불일치` | 방향은 맞지만 서버 에러 매핑과 동시성 검증 범위가 기준에 못 미친다. |
| `origin/feat/chaekang` | `계획 불일치` | 설계와 검증 의도는 가장 강하지만 실제 서버 테스트가 깨져 있다. |
| `origin/hg` | `계획 불일치` | 엔진 구조와 부하 테스트 문서는 좋지만 서버 상태코드 계약이 기준과 어긋난다. |
| `origin/km` | `부분 구현` | 현재 기준에 가장 가깝지만 load test 근거와 low-level error plumbing이 부족하다. |

현재 기준 문서에 가장 가까운 구현은 `origin/km` 이다.  
다만 `Gate` 항목을 모두 `Pass` 하지는 못해 `계획대로 구현됨` 으로는 판정하지 않았다.

---

## Branch별 상세 평가

## `origin/eunyeol`

### 최종 판정

`계획 불일치`

### 요약

- `rwlock`, queue, engine entrypoint, 결과 구조체는 들어가 있다.
- 하지만 서버가 DB 실패를 사실상 전부 `400` 으로 처리한다.
- 동시성 검증이 기준 문서가 요구한 5개 핵심 시나리오를 충분히 덮지 못한다.
- low-level runtime/tokenizer 경로에 직접 `stderr` 출력이 남아 있다.

### 강점

- `engine_execute_sql()` 중심 경계가 존재한다.
  - 근거: [src/engine.c](/tmp/jdb_eval_eunyeol/src/engine.c:10)
- per-table `pthread_rwlock_t` 와 read/write API가 있다.
  - 근거: [src/table_runtime.c](/tmp/jdb_eval_eunyeol/src/table_runtime.c:14)
- worker 8, queue 32 구조가 구현돼 있다.
  - 근거: [src/api_server.h](/tmp/jdb_eval_eunyeol/src/api_server.h:5)

### 주요 실패 이유

- `C18` Fail: 서버가 성공/실패를 `200/400` 으로만 단순화한다.
  - 근거: [src/api_server.c](/tmp/jdb_eval_eunyeol/src/api_server.c:623)
- `C07`, `C10` Partial: low-level helper가 여전히 직접 `stderr` 로 에러를 출력한다.
  - 근거: [src/table_runtime.c](/tmp/jdb_eval_eunyeol/src/table_runtime.c:77)
  - 근거: [src/tokenizer.c](/tmp/jdb_eval_eunyeol/src/tokenizer.c:24)
- `C20` Fail: 자동 테스트가 same-table read/read, read/write, write/write, missing-table flood를 모두 명시적으로 검증하지 않는다.
  - API 테스트 범위 근거: [tests/test_api_server.c](/tmp/jdb_eval_eunyeol/tests/test_api_server.c:85)
  - table concurrency 테스트 근거: [tests/test_table_runtime_concurrency.c](/tmp/jdb_eval_eunyeol/tests/test_table_runtime_concurrency.c:1)
- `C21` Fail: load test 문서/스크립트가 없다.

### Gate 항목 판정 요약

| 항목 | 판정 | 메모 |
| --- | --- | --- |
| `C02` tokenizer stateless | `Pass` | 전역 cache 없음 |
| `C03` per-table rwlock | `Pass` | 구현됨 |
| `C04` read/write registry 분리 | `Pass` | read/write API 분리 |
| `C05` multi-table isolation | `Pass` | 기본 구조 충족 |
| `C06` QueryResult 구조화 | `Pass` | 구현됨 |
| `C07` stdout/stderr 분리 | `Partial` | low-level stderr 잔존 |
| `C08` engine entrypoint | `Pass` | 구현됨 |
| `C09` lock lifetime | `Pass` | 결과 구조체 사용 |
| `C10` structured error | `Partial` | 일부 경로는 low-level stderr 의존 |
| `C11` read/read 병렬 | `Partial` | 정적 근거는 있으나 동적 증거 부족 |
| `C12` read/write, write/write 정합성 | `Partial` | 일부만 검증 |
| `C13` cross-table isolation | `Partial` | 기본 증거는 있으나 통합 검증 약함 |
| `C15` thread pool | `Pass` | worker/queue 구조 있음 |
| `C16` bounded queue fail-fast | `Pass` | 구현됨 |
| `C17` thin HTTP layer | `Pass` | 대체로 충족 |
| `C18` HTTP 응답 매핑 | `Fail` | 세분화 부족 |
| `C19` shutdown ordering | `Pass` | 구현 존재 |
| `C20` 5개 핵심 동시성 시나리오 | `Fail` | 테스트 범위 부족 |
| `C21` load test 해석 기준 | `Fail` | 문서/스크립트 없음 |

---

## `origin/feat/chaekang`

### 최종 판정

`계획 불일치`

### 요약

- branch 중 구현 의도와 테스트 범위는 가장 공격적이다.
- HTTP status mapping도 가장 세밀하게 시도한다.
- 하지만 자체 서버 테스트가 실제로 실패한다.
- 이 상태에서는 서버 관련 Gate 항목을 `Pass` 로 줄 수 없다.

### 강점

- 서버 상태코드 매핑이 `400/404/409/500/503` 을 구분하려고 설계돼 있다.
  - 근거: [src/server.c](/tmp/jdb_eval_chaekang/src/server.c:549)
- 별도 `test_server.c` 와 `test_engine_concurrency.c` 가 있다.
  - 근거: [tests/test_server.c](/tmp/jdb_eval_chaekang/tests/test_server.c:560)
- same-table, cross-table, overload까지 서버 관점 테스트를 시도했다.

### 주요 실패 이유

- 가장 큰 문제는 동적 증거 실패다.
  - `make tests` 결과 `test_server` 실패
  - 실패 진입점 근거: [tests/test_server.c](/tmp/jdb_eval_chaekang/tests/test_server.c:561)
- 따라서 `C15`~`C18` 서버 Gate는 설계가 아니라 실제 동작 기준으로 `Pass` 불가다.
- `C21` Partial: load test 문서/스크립트가 없다.

### 추가 메모

- 코드만 보면 branch 방향은 가장 좋다.
- 하지만 평가 기준 문서는 "코드가 있어 보임"이 아니라 "테스트와 행위까지 맞는가"를 보므로, 현재 상태에선 최종 통과 불가다.

### Gate 항목 판정 요약

| 항목 | 판정 | 메모 |
| --- | --- | --- |
| `C02` tokenizer stateless | `Pass` | 전역 cache 없음 |
| `C03` per-table rwlock | `Pass` | 구현됨 |
| `C04` read/write registry 분리 | `Pass` | missing read 처리 포함 |
| `C05` multi-table isolation | `Pass` | 테스트 포함 |
| `C06` QueryResult 구조화 | `Pass` | 구현됨 |
| `C07` stdout/stderr 분리 | `Pass` | 핵심 경로는 구조화됨 |
| `C08` engine entrypoint | `Pass` | 구현됨 |
| `C09` lock lifetime | `Pass` | 결과 구조체 기반 |
| `C10` structured error | `Pass` | 상태코드 분기까지 설계 |
| `C11` read/read 병렬 | `Pass` | 동시성 테스트 의도 강함 |
| `C12` read/write, write/write 정합성 | `Pass` | 테스트 의도 강함 |
| `C13` cross-table isolation | `Pass` | 테스트 의도 강함 |
| `C15` thread pool | `Fail` | 실제 서버 테스트 실패로 동작 확정 불가 |
| `C16` bounded queue fail-fast | `Fail` | 서버 테스트 실패로 동작 확정 불가 |
| `C17` thin HTTP layer | `Partial` | 구조는 맞으나 실제 서버 검증 실패 |
| `C18` HTTP 응답 매핑 | `Fail` | 구현 의도는 좋지만 서버 테스트 실패 |
| `C19` shutdown ordering | `Partial` | 코드상 정리되나 서버 통합 실패 |
| `C20` 5개 핵심 동시성 시나리오 | `Partial` | 상당수 시도했지만 서버 통합 실패 |
| `C21` load test 해석 기준 | `Partial` | 문서/스크립트 없음 |

---

## `origin/hg`

### 최종 판정

`계획 불일치`

### 요약

- 코어 엔진 구조는 현재 작업 브랜치와 가장 유사하다.
- `k6` 문서와 스크립트가 있어 load test 해석 기준도 갖췄다.
- 하지만 서버가 engine 실패를 사실상 `400` 으로만 내려 `C18` 을 어긴다.
- 자동 테스트가 기준 문서의 5개 필수 동시성 시나리오를 모두 덮는 것은 아니다.

### 강점

- load test 문서와 스크립트가 있다.
  - 근거: [docs/k6_load_test.md](/tmp/jdb_eval_hg/docs/k6_load_test.md:1)
  - 근거: [loadtests/k6/mixed_same_table_read_read.js](/tmp/jdb_eval_hg/loadtests/k6/mixed_same_table_read_read.js:1)
- worker 8, queue 32, 503 fail-fast가 구현돼 있다.
  - 근거: [src/server.c](/tmp/jdb_eval_hg/src/server.c:21)
  - 근거: [src/server.c](/tmp/jdb_eval_hg/src/server.c:921)
- `make tests` 는 모두 통과했다.

### 주요 실패 이유

- `C18` Fail: `engine_execute_sql()` 실패면 HTTP status를 무조건 `400` 으로 보낸다.
  - 근거: [src/server.c](/tmp/jdb_eval_hg/src/server.c:797)
- `C20` Partial: 기준 문서가 요구한 5개 동시성 시나리오를 자동 테스트가 모두 직접 덮는 것은 아니다.
  - table concurrency 테스트는 주로 cross-table insert 중심
  - 근거: [tests/test_table_runtime_concurrency.c](/tmp/jdb_eval_hg/tests/test_table_runtime_concurrency.c:1)
- `C21` Pass 는 가능하지만, 문서 중심 증거라 실제 k6 실행 기록이 남아 있진 않다.

### Gate 항목 판정 요약

| 항목 | 판정 | 메모 |
| --- | --- | --- |
| `C02` tokenizer stateless | `Pass` | 전역 cache 없음 |
| `C03` per-table rwlock | `Pass` | 구현됨 |
| `C04` read/write registry 분리 | `Pass` | 구현됨 |
| `C05` multi-table isolation | `Pass` | 구현됨 |
| `C06` QueryResult 구조화 | `Pass` | 구현됨 |
| `C07` stdout/stderr 분리 | `Pass` | 핵심 엔진 경로 충족 |
| `C08` engine entrypoint | `Pass` | 구현됨 |
| `C09` lock lifetime | `Pass` | 구조상 충족 |
| `C10` structured error | `Pass` | 결과 구조체 있음 |
| `C11` read/read 병렬 | `Partial` | load test 문서는 있으나 자동 테스트 직접 증거 약함 |
| `C12` read/write, write/write 정합성 | `Partial` | 일부 증거 있음 |
| `C13` cross-table isolation | `Pass` | 테스트/문서 근거 있음 |
| `C15` thread pool | `Pass` | 구현됨 |
| `C16` bounded queue fail-fast | `Pass` | 구현됨 |
| `C17` thin HTTP layer | `Pass` | 구현됨 |
| `C18` HTTP 응답 매핑 | `Fail` | engine 실패가 사실상 전부 400 |
| `C19` shutdown ordering | `Pass` | 구현됨 |
| `C20` 5개 핵심 동시성 시나리오 | `Partial` | 전부 자동 검증하진 않음 |
| `C21` load test 해석 기준 | `Pass` | 문서와 스크립트 존재 |

---

## `origin/km`

### 최종 판정

`부분 구현`

### 요약

- 현재 평가 기준에 가장 가까운 branch다.
- `engine_execute_sql()` 중심 경계, per-table `rwlock`, queue 32, worker 8, 503 fail-fast 모두 들어가 있다.
- `tests/test_engine_concurrency.c` 가 기준 문서의 5개 핵심 동시성 시나리오를 거의 정확히 덮는다.
- 다만 load test 문서/스크립트가 없고, low-level helper의 직접 `stderr` 출력이 남아 있다.
- 서버 상태코드도 `404` 외에는 대부분 `400` 으로 단순화되어 있다.

### 강점

- 5개 핵심 동시성 시나리오를 하나의 테스트에서 직접 검증한다.
  - missing table flood: [tests/test_engine_concurrency.c](/tmp/jdb_eval_km/tests/test_engine_concurrency.c:117)
  - same-table read/read: [tests/test_engine_concurrency.c](/tmp/jdb_eval_km/tests/test_engine_concurrency.c:134)
  - same-table read/write: [tests/test_engine_concurrency.c](/tmp/jdb_eval_km/tests/test_engine_concurrency.c:150)
  - same-table write/write: [tests/test_engine_concurrency.c](/tmp/jdb_eval_km/tests/test_engine_concurrency.c:156)
  - cross-table isolation: [tests/test_engine_concurrency.c](/tmp/jdb_eval_km/tests/test_engine_concurrency.c:163)
- tokenizer cache는 제거됐고 호환용 API만 no-op 으로 남아 있다.
  - 근거: [src/tokenizer.c](/tmp/jdb_eval_km/src/tokenizer.c:321)
- 서버는 `405`, `404`, `413`, `503` 을 명시적으로 구분한다.
  - 근거: [src/server.c](/tmp/jdb_eval_km/src/server.c:730)

### 보완이 필요한 이유

- `C07`, `C10` Partial: tokenizer/runtime low-level helper가 여전히 `stderr` 출력에 의존한다.
  - 근거: [src/tokenizer.c](/tmp/jdb_eval_km/src/tokenizer.c:24)
  - 근거: [src/table_runtime.c](/tmp/jdb_eval_km/src/table_runtime.c:79)
- `C18` Partial: DB 에러 상태코드 매핑이 `404` 외에는 대부분 `400` 이다.
  - 근거: [src/server.c](/tmp/jdb_eval_km/src/server.c:717)
- `C21` Partial: load test 문서와 스크립트가 없다.

### Gate 항목 판정 요약

| 항목 | 판정 | 메모 |
| --- | --- | --- |
| `C02` tokenizer stateless | `Pass` | cache 제거, no-op 호환 API |
| `C03` per-table rwlock | `Pass` | 구현됨 |
| `C04` read/write registry 분리 | `Pass` | 구현됨 |
| `C05` multi-table isolation | `Pass` | 구현됨 |
| `C06` QueryResult 구조화 | `Pass` | 구현됨 |
| `C07` stdout/stderr 분리 | `Partial` | low-level stderr 남음 |
| `C08` engine entrypoint | `Pass` | 구현됨 |
| `C09` lock lifetime | `Pass` | 구조상 충족 |
| `C10` structured error | `Partial` | low-level error plumbing 미흡 |
| `C11` read/read 병렬 | `Pass` | concurrency test 포함 |
| `C12` read/write, write/write 정합성 | `Pass` | concurrency test 포함 |
| `C13` cross-table isolation | `Pass` | concurrency test 포함 |
| `C15` thread pool | `Pass` | 구현됨 |
| `C16` bounded queue fail-fast | `Pass` | 구현됨 |
| `C17` thin HTTP layer | `Pass` | 구현됨 |
| `C18` HTTP 응답 매핑 | `Partial` | `404` 외 세분화 약함 |
| `C19` shutdown ordering | `Pass` | 구현됨 |
| `C20` 5개 핵심 동시성 시나리오 | `Pass` | 테스트로 직접 덮음 |
| `C21` load test 해석 기준 | `Partial` | 문서/스크립트 없음 |

---

## 결론

현재 remote branch 중 기준 문서에 가장 가까운 구현은 `origin/km` 이다.

다만 다음 이유로 아직 `계획대로 구현됨` 으로는 볼 수 없다.

- low-level helper의 직접 `stderr` 출력 잔존
- 서버의 DB 에러 상태코드 매핑 단순화
- load test 문서와 실행 근거 부재

`origin/feat/chaekang` 은 설계와 테스트 의도는 가장 강하지만,
서버 통합 테스트가 깨져 있어 현재 상태에선 통과 후보로 보기 어렵다.

`origin/hg`, `origin/eunyeol` 은 구조는 상당 부분 맞지만,
평가 기준 문서가 요구한 서버 응답 계약과 동시성 검증 범위를 아직 충분히 만족하지 못한다.
