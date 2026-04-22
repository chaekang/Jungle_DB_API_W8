# Concurrent Server Refactor Evaluation Criteria

## 1. 목적

이 문서는 [`docs/concurrent_server_refactor_plan.md`](./concurrent_server_refactor_plan.md) 의 계획대로 구현되었는지를 판단할 때 사용할 **평가 항목과 판정 기준만** 정의한다.

이 문서는 다음을 한다.

- 무엇을 평가 대상에 포함해야 하는지 정한다.
- 각 항목을 어떤 증거로 판정해야 하는지 정한다.
- 어떤 경우를 `구현 완료`, `부분 구현`, `불일치`, `판정 보류`로 볼지 정한다.

이 문서는 다음을 하지 않는다.

- 현재 코드가 실제로 통과했는지 판정하지 않는다.
- 개선안이나 구현안을 제시하지 않는다.
- 성능 수치 자체를 목표값으로 확정하지 않는다.

---

## 2. 평가 원칙

### 2.1 확정된 계획만 Pass/Fail 대상으로 본다

`concurrent_server_refactor_plan.md` 안에는 이미 확정된 항목과 아직 흔들리는 항목이 섞여 있다.  
평가자는 반드시 이 둘을 분리해야 한다.

`Pass/Fail` 로 판정해야 하는 항목:

- 섹션 `3.1` 에서 비교적 확정된 것으로 정리한 항목
- 섹션 `4` 에서 이 문서가 채택한 기본 방향
- 섹션 `6`, `9`, `10`, `12` 에서 완료 기준까지 명시한 항목

자동 탈락 사유로 쓰면 안 되는 항목:

- API shape 자체가 `/query` 인지 `/insert`, `/select` 인지의 선택
- persistence 포함 여부
- index 단위 lock 도입 여부
- generic router, non-blocking I/O, metrics 같은 후순위 개선

위 항목은 계획 문서상 **미확정 또는 의도적 보류** 이므로, 팀이 따로 범위를 확정하지 않았다면 `불일치`가 아니라 `판정 보류` 또는 `평가 제외` 로 처리해야 한다.

### 2.2 코드 존재만으로는 Pass가 아니다

어떤 함수명이나 자료구조가 보인다고 해서 구현 완료로 보지 않는다.  
최소한 아래 세 층위 중 두 층 이상에서 증거가 있어야 한다.

- 정적 증거: 헤더, 자료구조, 호출 경로, 전역 상태 유무
- 동적 증거: 단위 테스트, 동시성 테스트, 부하 테스트
- 행위 증거: 실제 요청 처리, shutdown, overload 시의 관찰 가능한 동작

특히 동시성 요구사항은 정적 코드만 읽고 `대충 맞아 보인다` 수준으로 통과시키면 안 된다.

### 2.3 서버용 correct behavior를 우선 판정한다

이 계획의 핵심 목표는 "concurrent server에 연결 가능한 DB 엔진" 이다.  
따라서 평가는 반드시 다음 질문에 집중해야 한다.

- 읽기/읽기 병렬이 실제로 가능한가
- 쓰기가 배타적으로 보호되는가
- 엔진이 HTTP를 모르고 구조화된 결과만 반환하는가
- 실패한 요청과 overload가 서버에서 안전하게 처리되는가

### 2.4 미구현과 의도적 제외를 구분한다

다음은 기본적으로 `미구현` 이 아니라 `범위 제외` 로 본다.

- persistence 재통합이 아직 없음
- tokenizer cache를 다시 살리지 않음
- index 단위 lock 없음
- generic router 없음
- non-blocking I/O 없음
- metrics endpoint 없음

단, 이런 제외 항목 때문에 핵심 설계가 깨졌다면 그때는 관련 핵심 항목에서 탈락 처리한다.

---

## 3. 판정 상태 정의

| 상태 | 의미 |
| --- | --- |
| `Pass` | 해당 항목의 설계, 동작, 증거가 모두 계획과 일치한다. |
| `Partial` | 방향은 맞지만 증거가 부족하거나 일부 완료 기준이 비어 있다. |
| `Fail` | 구현이 계획과 직접 충돌하거나, 완료 기준을 명확히 어긴다. |
| `Hold` | 계획 문서상 아직 확정되지 않은 항목이라 현재로서는 판정하면 안 된다. |

최종적으로 `계획대로 구현되었다` 고 말하려면 최소한 아래 조건이 필요하다.

- `Gate` 등급 항목이 모두 `Pass` 여야 한다.
- `Gate` 항목에 `Partial` 이 하나라도 있으면 최종 판정은 `부분 구현` 이다.
- `Gate` 항목에 `Fail` 이 하나라도 있으면 최종 판정은 `계획 불일치` 다.
- `Advisory` 항목은 최종 탈락 사유가 아니라, 품질 메모로만 남긴다.

---

## 4. 항목별 평가 기준

## C01. 평가 기준선이 올바르게 설정되었는가

- 중요도: `Gate`
- 근거 계획: `3.1`, `3.2`, `4`, `7`, `8.3`, `11`
- 평가 질문: 평가자가 확정 항목과 보류 항목을 섞지 않고 판정하는가.
- Pass 기준:
  - table-level `pthread_rwlock_t`, worker 8, queue 32, fail-fast overload, engine/HTTP 분리, memory-runtime 우선이 필수 항목으로 분류돼 있다.
  - API shape, persistence, finer lock granularity는 `Hold` 또는 `평가 제외` 로 다뤄진다.
- Partial 기준:
  - 확정/보류 구분은 했지만, 일부 항목의 경계가 모호해 같은 구현을 평가자마다 다르게 해석할 수 있다.
- Fail 기준:
  - `/query` 채택만으로 탈락시킨다.
  - persistence 부재만으로 탈락시킨다.
  - optional improvement 미구현을 핵심 실패로 취급한다.
- 필수 증거:
  - 평가 체크리스트나 리뷰 문서에 확정/보류 구분이 명시돼 있어야 한다.

## C02. tokenizer 경로가 서버 관점에서 무상태(stateless)에 가까운가

- 중요도: `Gate`
- 근거 계획: `2.2 문제 2`, `4.3`, `6.3`, `9.1`
- 평가 질문: `tokenizer_tokenize()` 경로가 입력 문자열과 지역 메모리만으로 동작하는가.
- Pass 기준:
  - tokenizer가 process-global mutable cache, hit counter, linked-list cache 같은 공유 가변 상태를 가지지 않는다.
  - tokenizer correctness를 위해 별도 전역 mutex가 필요하지 않다.
  - server worker가 동시에 tokenize 해도 tokenizer 내부 상태 경쟁이 생기지 않는다.
- Partial 기준:
  - 전역 상태는 제거됐지만, thread-safety를 증명하는 테스트나 명확한 정적 근거가 약하다.
- Fail 기준:
  - `tokenizer_cache_head`, cache count, move-to-front cache, eviction cache 같은 공유 mutable 상태가 남아 있다.
  - tokenizer 보호를 위해 서버나 엔진 바깥에서 별도 coarse lock을 잡아야 한다.
- 필수 증거:
  - `src/tokenizer.c`, `src/tokenizer.h` 에 전역 mutable cache가 없는지 확인
  - tokenizer 테스트가 기존 동작을 유지하는지 확인
- 대표 탈락 신호:
  - `static` 전역 리스트나 카운터가 존재한다.
  - cache hit율 집계를 위해 공유 상태를 계속 갱신한다.

## C03. 테이블별 락 primitive가 `pthread_rwlock_t` 로 정착했는가

- 중요도: `Gate`
- 근거 계획: `2.2 문제 1`, `4.2`, `6.1`, `9.2`, `12`
- 평가 질문: 같은 테이블 read/read 병렬과 write 배타를 구현할 수 있는 락 구조인가.
- Pass 기준:
  - 각 `TableRuntimeEntry` 가 `pthread_rwlock_t` 를 가진다.
  - registry 보호용 락과 테이블 데이터 보호용 락이 역할상 분리돼 있다.
  - 읽기 경로는 read lock, 쓰기 경로는 write lock을 사용한다.
- Partial 기준:
  - rwlock은 도입됐지만, 일부 경로가 여전히 write lock만 사용하거나 호출 규약이 불명확하다.
- Fail 기준:
  - 같은 테이블 접근을 여전히 `pthread_mutex_t` 하나로 직렬화한다.
  - DB 전체에 단일 lock 하나만 두고 테이블별 병렬성을 잃는다.
- 필수 증거:
  - `src/table_runtime.c`, `src/table_runtime.h` 의 자료구조와 공개 API
  - 같은 테이블 read/read, read/write, write/write 테스트
- 대표 탈락 신호:
  - read API가 존재하지 않거나 read도 write lock을 잡는다.

## C04. read path와 write path의 registry 의미가 분리되었는가

- 중요도: `Gate`
- 근거 계획: `2.2 문제 4`, `6.2`, `9.2`
- 평가 질문: 읽기 miss가 registry를 오염시키지 않는가.
- Pass 기준:
  - read acquire는 없는 테이블에 대해 실패한다.
  - write acquire만 on-demand create를 수행한다.
  - 잘못된 테이블 이름으로 읽기 요청을 반복해도 빈 runtime entry가 누적되지 않는다.
- Partial 기준:
  - read miss는 실패하지만, registry 오염 방지에 대한 테스트나 관찰 가능한 증거가 없다.
- Fail 기준:
  - read path가 내부적으로 create를 호출한다.
  - missing table read flood 이후 registry가 계속 커질 수 있다.
- 필수 증거:
  - acquire API의 코드 경로
  - missing table read flood 테스트 또는 동일 수준의 증거
- 대표 탈락 신호:
  - `table_runtime_acquire_*()` 하나로 read/write를 모두 처리하면서 `없으면 생성` 한다.

## C05. multi-table 상태 보존과 cross-table 독립성이 유지되는가

- 중요도: `Gate`
- 근거 계획: `2.1`, `4.1`, `6.1`, `9.5`, `12`
- 평가 질문: 한 테이블의 작업이 다른 테이블 runtime 상태를 덮어쓰거나 손상시키지 않는가.
- Pass 기준:
  - 서로 다른 테이블이 독립된 runtime state, `next_id`, schema, index를 유지한다.
  - 테이블 A 작업 후에도 테이블 B 상태가 보존된다.
  - cross-table 병렬 처리 중에도 데이터가 섞이지 않는다.
- Partial 기준:
  - 독립 registry는 있으나 cross-table 동시성 또는 유지성 테스트가 약하다.
- Fail 기준:
  - active runtime 재사용 때문에 다른 테이블 접근 시 기존 상태가 날아간다.
  - cross-table 작업이 `next_id`, row, schema, index를 공유한다.
- 필수 증거:
  - table registry 구조
  - multi-table unit test
  - cross-table concurrency/load test

## C06. executor/engine 결과가 구조화된 `QueryResult` 로 귀결되는가

- 중요도: `Gate`
- 근거 계획: `2.2 문제 3`, `4.4`, `6.4`, `8.4`, `9.3`, `12`
- 평가 질문: 서버와 CLI가 문자열 출력이 아니라 구조화된 결과를 공유하는가.
- Pass 기준:
  - 성공/실패 여부가 구조체 필드로 표현된다.
  - `message`, `error`, `columns`, `rows` 정도의 최소 필드가 존재한다.
  - `SELECT` 결과는 런타임 데이터를 복사한 뒤 결과 구조체가 소유한다.
  - 결과 해제 API가 존재한다.
- Partial 기준:
  - 결과 구조체는 있으나 row ownership 이 불명확하거나 일부 경로가 여전히 출력 의존적이다.
- Fail 기준:
  - 서버가 stdout 텍스트를 파싱해 응답을 만든다.
  - `SELECT` 결과가 runtime 내부 row 포인터를 직접 노출한다.
  - 결과 메모리 해제 규약이 없다.
- 필수 증거:
  - `src/query_result.h`, `src/query_result.c`
  - executor/engine 호출부
- 대표 탈락 신호:
  - `printf` 출력이 query 결과의 정식 전달 방식으로 남아 있다.

## C07. executor와 engine이 `stdout/stderr` 에서 분리되었는가

- 중요도: `Gate`
- 근거 계획: `2.2 문제 3`, `6.4`, `6.7`, `9.3`, `9.4`
- 평가 질문: DB 실행 자체가 콘솔 출력 없이 완료되는가.
- Pass 기준:
  - query success/failure 의미가 출력이 아니라 반환값과 결과 구조체에 담긴다.
  - CLI 출력은 adapter 층에서만 수행된다.
  - engine/executor가 결과 전달을 위해 `stdout` 또는 `stderr` 에 의존하지 않는다.
- Partial 기준:
  - 정상 결과는 구조화됐지만 일부 오류가 여전히 `stderr` 에만 기록된다.
- Fail 기준:
  - insert 성공 메시지를 executor가 직접 출력한다.
  - select 결과 표를 executor가 직접 찍는다.
  - 오류 메시지가 결과 구조체 없이 콘솔에만 출력된다.
- 필수 증거:
  - `src/executor.c`, `src/engine.c`, `src/main.c`
  - 출력 함수 검색 결과

## C08. 엔진 경계가 `engine_execute_sql()` 같은 단일 entrypoint로 고정되었는가

- 중요도: `Gate`
- 근거 계획: `4.4`, `6.5`, `8.3`, `9.4`, `12`
- 평가 질문: 서버와 CLI가 tokenizer/parser/executor 내부를 직접 알지 않고 같은 엔진 진입점을 쓰는가.
- Pass 기준:
  - public engine API가 SQL 문자열과 `QueryResult` 중심으로 정의돼 있다.
  - server와 CLI가 같은 엔진 API를 사용한다.
  - lock 획득, parse, execute, error packaging이 엔진 경계 안쪽에 모인다.
- Partial 기준:
  - engine API는 있지만 일부 호출 경로가 여전히 tokenizer/parser/executor를 직접 호출한다.
- Fail 기준:
  - HTTP 계층이 tokenizer/parser를 직접 다룬다.
  - UI 계층마다 서로 다른 DB 호출 규약을 쓴다.
- 필수 증거:
  - `src/engine.h`, `src/engine.c`
  - `src/main.c`, `src/server.c` 호출 경로

## C09. DB lock의 생애주기가 네트워크 응답보다 짧은가

- 중요도: `Gate`
- 근거 계획: `6.6`, `9.3`, `9.4`, `10.4`, `12`
- 평가 질문: DB lock 이 JSON 직렬화나 socket write 동안 유지되지 않는가.
- Pass 기준:
  - 읽기/쓰기 락은 runtime 접근과 결과 복사 직후 해제된다.
  - `QueryResult` 는 lock 해제 뒤에도 안전하게 사용할 수 있다.
  - HTTP 응답 생성과 송신은 lock 밖에서 이뤄진다.
- Partial 기준:
  - 구조상 그럴 가능성이 높지만, row ownership 또는 호출 순서 증거가 약하다.
- Fail 기준:
  - 서버가 table handle 을 잡은 채 JSON 직렬화/응답 송신을 한다.
  - 결과가 runtime 내부 포인터를 가리켜 lock을 오래 붙잡아야 한다.
- 필수 증거:
  - executor와 server 호출 체인
  - 결과 메모리 복사 여부
  - read/write conflict 테스트나 코드 리뷰 근거

## C10. 에러가 구조화되어 상위 계층까지 전달되는가

- 중요도: `Gate`
- 근거 계획: `6.7`, `8.4`, `9.3`, `9.4`
- 평가 질문: tokenizer/parser/runtime/unsupported statement 실패가 일관된 에러 필드로 전달되는가.
- Pass 기준:
  - 최소한 아래 실패가 `result.error` 같은 구조화된 필드에 담긴다.
  - tokenize 실패
  - parse 실패
  - missing table 실패
  - unsupported statement 실패
  - 상위 계층은 이 에러 필드를 그대로 CLI 또는 HTTP 응답으로 사용한다.
- Partial 기준:
  - 일부 실패는 구조화돼 있으나 몇몇 경로는 빈 에러 또는 generic error로 떨어진다.
- Fail 기준:
  - engine failure 후 사용자에게 전달할 에러 메시지가 비어 있다.
  - 오류 표현이 stderr 로그에만 남고 API 응답에는 보존되지 않는다.
- 필수 증거:
  - 에러 버퍼 사용 경로
  - 엔진 실패 테스트
  - HTTP error body 또는 CLI error 출력이 동일한 결과원을 쓰는지 확인

## C11. 같은 테이블 read/read 병렬성이 실제로 성립하는가

- 중요도: `Gate`
- 근거 계획: `2.2 문제 1`, `4.2`, `9.2`, `9.5`, `12`
- 평가 질문: 같은 테이블의 두 `SELECT` 가 직렬화되지 않고 함께 진행될 수 있는가.
- Pass 기준:
  - 코드상 read lock 이 read/read 공유를 허용한다.
  - deadlock 없이 두 요청이 모두 성공한다.
  - 부하 테스트 또는 동등한 동적 증거에서 fast lookup latency가 slow scan latency에 끌려가며 완전히 직렬화되지 않는다.
- Partial 기준:
  - 코드상 가능해 보이지만, 같은 테이블 read/read 동적 증거가 없다.
- Fail 기준:
  - 같은 테이블 select가 write lock 또는 mutex 때문에 사실상 직렬화된다.
  - mixed read/read 상황에서 빠른 조회가 느린 조회와 같은 수준으로 막힌다.
- 필수 증거:
  - same-table `SELECT + SELECT` 테스트
  - 필요 시 baseline 대비 mixed latency 비교
- 권장 해석:
  - 절대 latency 수치보다 "baseline에 더 가까운가, slow scan에 더 가까운가" 를 본다.

## C12. 같은 테이블 read/write 와 write/write 의 정합성이 보장되는가

- 중요도: `Gate`
- 근거 계획: `3.1`, `4.2`, `6.1`, `9.2`, `9.5`, `12`
- 평가 질문: writer가 reader 및 다른 writer 와 충돌해도 데이터 정합성이 깨지지 않는가.
- Pass 기준:
  - `SELECT + INSERT` 동시 실행에서 crash, deadlock, torn read, 손상된 row 가 없다.
  - `INSERT + INSERT` 동시 실행에서 id 중복이 없다.
  - row count, `next_id`, index mapping 이 일관된다.
- Partial 기준:
  - 동시 write 보호는 있어 보이지만, 동시성 테스트가 충분하지 않다.
- Fail 기준:
  - id 중복이 발생한다.
  - row_count 와 실제 row 데이터가 맞지 않는다.
  - B+Tree index 가 잘못된 row slot 을 가리킨다.
- 필수 증거:
  - same-table `SELECT + INSERT` 테스트
  - same-table `INSERT + INSERT` 테스트
  - row/index integrity assertions
- 판단 주의:
  - 이 계획은 MVCC 나 snapshot isolation을 요구하지 않는다.
  - 따라서 평가는 "완벽한 snapshot" 이 아니라 "락 기반 정합성" 여부에 맞춰야 한다.

## C13. cross-table 동시성 격리가 유지되는가

- 중요도: `Gate`
- 근거 계획: `2.1`, `4.1`, `4.2`, `9.5`, `12`
- 평가 질문: 큰 작업이 테이블 A 에서 진행돼도 테이블 B 는 같은 이유로 막히지 않는가.
- Pass 기준:
  - 다른 테이블 read/write 는 동일 테이블 lock 경합 없이 독립적으로 진행된다.
  - cross-table 부하 상황에서 다른 테이블의 빠른 조회가 baseline에 가깝게 유지된다.
- Partial 기준:
  - cross-table unit test 는 있으나, 실제 요청 부하 기준의 관찰 증거가 약하다.
- Fail 기준:
  - 전역 DB lock 때문에 다른 테이블 요청도 함께 막힌다.
  - cross-table 작업에서 상태가 섞이거나 latency가 동일 테이블 경합처럼 악화된다.
- 필수 증거:
  - 서로 다른 테이블 동시성 테스트
  - cross-table isolation 부하 테스트 또는 동등한 통합 증거

## C14. CLI 와 서버가 같은 엔진 경계와 같은 결과 모델을 공유하는가

- 중요도: `Gate`
- 근거 계획: `4.4`, `6.5`, `8.3`, `9.3`, `9.4`, `10.4`, `12`
- 평가 질문: CLI 전용 경로와 서버 전용 경로가 DB 실행 규약을 따로 만들지 않았는가.
- Pass 기준:
  - CLI 는 `engine_execute_sql()` 호출 후 텍스트 렌더링만 담당한다.
  - 서버는 같은 결과 구조체를 JSON 으로 직렬화한다.
  - business logic 이 CLI/server 에 중복되지 않는다.
- Partial 기준:
  - 대부분 공유하지만 일부 statement 처리나 에러 매핑이 한쪽에만 별도로 있다.
- Fail 기준:
  - CLI 와 서버가 서로 다른 executor 경로를 사용한다.
  - 한쪽만 지원하는 DB 실행 규약 때문에 동작이 분기된다.
- 필수 증거:
  - `src/main.c`, `src/server.c`, `src/engine.c`

## C15. 서버의 thread pool 구조가 계획과 일치하는가

- 중요도: `Gate`
- 근거 계획: `3.1`, `8.1`, `8.2`, `10.1`, `10.2`
- 평가 질문: main thread 와 worker 의 책임 분리가 계획과 일치하는가.
- Pass 기준:
  - worker 수가 초기값 8로 고정돼 있다.
  - main thread 는 accept 와 enqueue 만 담당한다.
  - worker 하나가 request parse, DB 호출, 응답 직렬화, socket write, close 를 끝까지 담당한다.
- Partial 기준:
  - thread pool 은 있으나 책임 분리가 다소 흐리거나 worker 수가 계획과 다르다.
- Fail 기준:
  - 요청마다 새 스레드를 생성한다.
  - main thread 가 query 실행까지 직접 담당한다.
  - parse 와 execute 가 서로 다른 worker 간에 분산된다.
- 필수 증거:
  - `src/server.c` 호출 흐름
  - worker 생성부와 request handling 경로

## C16. bounded queue 와 fail-fast overload 정책이 구현되었는가

- 중요도: `Gate`
- 근거 계획: `3.1`, `8.2`, `10.1`, `10.5`
- 평가 질문: queue capacity 32와 queue full 즉시 실패가 실제 동작인가.
- Pass 기준:
  - queue capacity 가 32 로 고정 또는 명확히 설정돼 있다.
  - queue full 시 요청을 큐에 넣지 않는다.
  - overload 응답이 즉시 반환된다.
  - overload 시 서버 전체가 멈추거나 crash 하지 않는다.
- Partial 기준:
  - bounded queue 는 있으나 overload 동작 증거가 약하다.
- Fail 기준:
  - queue 가 사실상 무한하다.
  - queue full 시 accept thread 가 오래 block 하며 밀어 넣는다.
  - overload 응답 없이 연결이 멎는다.
- 필수 증거:
  - queue 자료구조와 push/pop 규약
  - overload 시나리오 테스트
- 대표 탈락 신호:
  - fail-fast 대신 대기열을 계속 늘린다.

## C17. HTTP 계층이 얇고 최소 범위로 제한되어 있는가

- 중요도: `Gate`
- 근거 계획: `4.4`, `8.1`, `8.3`, `10.3`, `10.4`
- 평가 질문: HTTP 파서가 과제 범위를 넘는 복잡성을 끌어들이지 않았는가.
- Pass 기준:
  - 초기 범위는 `POST`, `Content-Length`, `JSON body` 정도로 제한된다.
  - 지원하지 않는 기능은 명시적으로 거부하거나 아예 구현하지 않는다.
  - DB 엔진은 HTTP status line, header, socket 을 모른다.
- Partial 기준:
  - 최소 파서는 구현됐지만, 일부 경계 조건 처리가 불명확하다.
- Fail 기준:
  - 엔진 코드가 HTTP 개념을 직접 다룬다.
  - 서버가 unsupported HTTP 기능을 암묵적으로 받아들여 오동작한다.
- 필수 증거:
  - `src/server.c`
  - request parse 에러 테스트

## C18. HTTP 응답 매핑이 구조화되고 일관적인가

- 중요도: `Gate`
- 근거 계획: `8.4`, `10.4`, `10.5`
- 평가 질문: 성공/실패/overload 가 예측 가능한 HTTP 응답으로 표현되는가.
- Pass 기준:
  - 성공 시 `success: true` 와 `message` 또는 `columns/rows` 가 내려간다.
  - 실패 시 `success: false` 와 `error` 가 내려간다.
  - queue full 은 `503` 으로 표현된다.
  - malformed request, wrong path, wrong method 같은 서버 계층 실패는 DB 계층 실패와 구분된다.
- Partial 기준:
  - JSON body 구조는 맞지만 status mapping 이 일부 모호하다.
- Fail 기준:
  - engine 실패가 `200` 성공 응답으로 내려간다.
  - overload 가 `503` 이 아닌 침묵, hang, crash 로 나타난다.
- 필수 증거:
  - response builder 코드
  - malformed request / overload 테스트
- 판단 주의:
  - `409` 사용 여부 자체는 필수 통과 항목이 아니다.
  - 중요한 것은 상태 코드와 body 가 모순되지 않는 것이다.

## C19. shutdown 순서와 cleanup 전제가 계획대로 보장되는가

- 중요도: `Gate`
- 근거 계획: `2.2 문제 5`, `10.5`
- 평가 질문: engine cleanup 이 active worker 종료 이후에만 수행되는가.
- Pass 기준:
  - listen 중단
  - accept loop 종료
  - worker join
  - 그 이후 cleanup
  - 위 순서가 코드나 운영 절차에서 명확하다.
- Partial 기준:
  - 대체로 맞지만 순서 보장이 코드상 약하거나 문서화가 부족하다.
- Fail 기준:
  - worker 실행 중 cleanup 이 먼저 호출될 수 있다.
  - shutdown 도중 새 요청을 받거나 dangling handle 이 남을 수 있다.
- 필수 증거:
  - `server_run()` 종료 경로
  - `engine_cleanup()` 호출 위치
  - shutdown 시나리오 검토

## C20. 동시성 테스트 셋이 계획된 다섯 가지 핵심 시나리오를 덮는가

- 중요도: `Gate`
- 근거 계획: `9.5`
- 평가 질문: 계획서가 반드시 필요하다고 한 시나리오가 실제 검증 범위에 포함돼 있는가.
- Pass 기준:
  - 같은 테이블 `SELECT + SELECT`
  - 같은 테이블 `SELECT + INSERT`
  - 같은 테이블 `INSERT + INSERT`
  - 다른 테이블 `SELECT + INSERT`
  - missing table read flood
  - 위 다섯 항목 각각에 대해 성공 조건과 무결성 조건이 명시돼 있다.
- Partial 기준:
  - 일부 시나리오는 있으나 다섯 가지 중 하나 이상이 빠져 있다.
- Fail 기준:
  - 다른 테이블 insert 테스트만 있고 같은 테이블 동시성 검증이 없다.
  - registry 오염 방지 검증이 빠져 있다.
- 필수 증거:
  - `tests/` 의 단위/통합 테스트
  - 필요 시 `loadtests/k6/` 시나리오

## C21. 부하 테스트 해석 기준이 상대 비교로 정의되어 있는가

- 중요도: `Gate`
- 근거 계획: `docs/k6_load_test.md`, `11`, `12`
- 평가 질문: 부하 테스트 결과를 환경 의존적인 절대값 대신 설계 의도에 맞는 상대 관계로 해석하는가.
- Pass 기준:
  - baseline fast lookup 과 mixed 상황의 fast lookup 을 비교한다.
  - same-table read/read 는 mixed 상황에서도 fast lookup 이 slow scan latency에 완전히 끌려가지 않는지를 본다.
  - cross-table isolation 은 다른 테이블 lookup 이 baseline에 더 가깝게 유지되는지를 본다.
  - overload 테스트는 일부 요청이 빠르게 `503` 되는지를 본다.
- Partial 기준:
  - 부하 테스트는 있으나 해석 기준이 모호하다.
- Fail 기준:
  - 단순 평균 latency 한 줄만 보고 read/read 병렬 여부를 단정한다.
  - 환경 잡음 때문에 의미 없는 절대 수치 하나로 합격/불합격을 정한다.
- 필수 증거:
  - load test 문서
  - baseline, mixed, overload 결과 비교 기록

## C22. 보류 항목과 후속 개선 항목을 핵심 구현 완료 조건에 섞지 않았는가

- 중요도: `Gate`
- 근거 계획: `7`, `8.3`, `11`
- 평가 질문: 계획이 의도적으로 미룬 항목 때문에 현재 구현을 잘못 탈락시키고 있지 않은가.
- Pass 기준:
  - persistence 부재는 기본적으로 `Fail` 사유가 아니다.
  - `/query` 와 기능별 endpoint 중 무엇을 택했는지는 엔진 경계가 유지되면 `Fail` 사유가 아니다.
  - tokenizer cache 부활, index-level lock, non-blocking I/O, metrics 부재는 `Fail` 사유가 아니다.
- Partial 기준:
  - 평가자 메모는 있으나 최종 판정 논리에 범위 밖 항목이 일부 섞여 있다.
- Fail 기준:
  - optional improvement 를 미구현으로 기록해 핵심 구현 실패로 분류한다.
- 필수 증거:
  - 최종 리뷰 문서 또는 평가 체크리스트

## C23. 구현이 계획의 "단순하고 읽기 쉬운 구조" 원칙을 크게 훼손하지 않았는가

- 중요도: `Advisory`
- 근거 계획: `1`, `4.5`, `11`
- 평가 질문: 핵심 흐름을 설명하기 어려운 과도한 추상화가 들어가지는 않았는가.
- Pass 기준:
  - 함수 이름만 봐도 요청 처리 흐름이 읽힌다.
  - 지금 쓰지 않는 전략 객체, 플러그인 레이어, 추상 팩토리 같은 구조가 없다.
  - 동시성 제어 위치가 소수의 명확한 함수로 모여 있다.
- Partial 기준:
  - 구현은 맞지만 읽기 부담이 다소 커졌다.
- Fail 기준:
  - 현재 범위에 필요 없는 일반화 때문에 엔진/서버 경계나 락 책임이 흐려진다.
- 필수 증거:
  - 코드 리뷰
  - 호출 경로 복잡도 확인
- 판정 주의:
  - 이 항목은 품질 판단용이다.
  - 단독으로 최종 탈락 사유가 되지 않는다.

---

## 5. 최종 판정 규칙

이 문서를 기준으로 최종 판정은 아래처럼 내린다.

- `계획대로 구현됨`
  - `Gate` 항목이 모두 `Pass`
- `부분 구현`
  - `Gate` 항목에 `Partial` 이 하나 이상 있고 `Fail` 은 없음
- `계획 불일치`
  - `Gate` 항목에 `Fail` 이 하나 이상 있음
- `판정 보류`
  - 핵심 항목이 아니라 plan 자체가 아직 확정하지 않은 범위를 두고 논쟁 중임

가장 중요한 해석 원칙은 아래 한 줄이다.

> 이 계획의 완료 여부는 "서버가 붙었는가" 만이 아니라, "그 서버가 계획한 동시성 모델과 엔진 경계를 실제로 지키는가" 로 판단해야 한다.
