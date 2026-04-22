# Concurrent Server Refactor Plan

## 1. 문서 목적

이 문서는 현재 코드베이스를 이번 주 과제인 "미니 DBMS - API 서버"에 맞게 재구성하기 위한 기준 문서다.

기존 구조는 CLI 중심 SQL 처리기와 테이블 런타임 구현에 초점이 맞춰져 있었고, 외부 클라이언트가 동시에 요청을 보내는 API 서버 과제를 바로 수행하기에는 몇 가지 구조적 한계가 있다.

이번 리팩터링의 목적은 다음과 같다.

1. 기존 SQL 처리기와 B+ 트리 인덱스를 최대한 재사용한다.
2. 외부 클라이언트가 HTTP API를 통해 DBMS 기능을 사용할 수 있게 만든다.
3. 스레드 풀 기반 병렬 요청 처리 구조를 도입한다.
4. 멀티 스레드 환경에서 안전하게 동작하는 DB 엔진 경계를 만든다.
5. "학습용 실험"보다 "이번 주 안에 구현 가능한 구조"를 우선한다.

즉, 이 문서는 단순한 아이디어 정리가 아니라 "지금 과제를 완성하기 위해 어떤 구조로 바꾸고 어떤 순서로 구현할지"를 정하는 실행 계획이다.

---

## 2. 과제 요구사항 재해석

과제 요구사항과 `goal.md`의 중점 포인트를 기준으로, 이번 과제에서 반드시 만족해야 하는 핵심은 아래와 같다.

### 2.1 필수 요구사항

- 구현 언어는 C 언어다.
- API 서버를 통해 외부 클라이언트가 DBMS 기능을 사용할 수 있어야 한다.
- 요청은 스레드 풀 기반으로 병렬 처리해야 한다.
- 이전 차수에서 만든 SQL 처리기와 B+ 트리 인덱스를 내부 엔진으로 재사용해야 한다.
- 단위 테스트와 API 기능 테스트가 가능해야 한다.

### 2.2 평가에서 중요한 포인트

- 멀티 스레드 환경에서 데이터 정합성이 깨지지 않아야 한다.
- 내부 DB 엔진과 외부 API 서버의 책임 분리가 명확해야 한다.
- 서버 아키텍처가 설명 가능해야 한다.
- 단순히 "동작만 하는 코드"가 아니라, 핵심 흐름을 팀이 설명할 수 있어야 한다.

### 2.3 이번 문서의 실전 해석

이번 과제는 "완전한 범용 DBMS"를 만드는 과제가 아니다.
이번 과제는 "기존 SQL 엔진을 안전하게 감싸는 concurrent API 서버"를 만드는 과제다.

따라서 우선순위는 다음과 같이 둔다.

1. 서버와 엔진의 연결 구조를 명확히 만든다.
2. 병렬 요청에서도 안전한 동작을 확보한다.
3. 발표와 데모가 가능한 API 흐름을 빠르게 완성한다.
4. 그 다음에 선택적 차별화 요소를 올린다.

---

## 3. 현재 구조의 문제와 리팩터링 필요성

현재 `main` 브랜치의 구조는 이전 단계 과제를 수행하기에는 충분히 의미 있는 발전이 있었지만, 이번 과제의 목표에는 그대로 맞지 않는다.

### 3.1 현재 구조의 장점

- SQL tokenizer / parser / executor 흐름이 이미 존재한다.
- 테이블별 runtime registry가 있어 multi-table 상태를 분리하는 방향으로 발전해 있다.
- B+ 트리 인덱스와 메모리 기반 테이블 런타임을 재사용할 수 있다.
- 기존 테스트 자산이 있어 회귀 검증 기반이 있다.

### 3.2 현재 구조의 한계

- executor가 결과를 구조체로 반환하지 않고 출력 중심으로 동작한다.
- tokenizer에 전역 mutable cache가 있어 멀티 스레드 환경에서 위험하다.
- 같은 테이블에 대한 동시 읽기/쓰기를 구분하는 락 모델이 부족하다.
- 서버가 붙어야 할 엔진 진입점이 하나로 정리되어 있지 않다.
- 현재 구조는 REPL/CLI에는 편하지만 API 서버에는 불리하다.

### 3.3 결론

이번 과제를 원활하게 수행하려면, 기존 SQL 처리기를 폐기할 필요는 없지만 "서버가 붙기 좋은 엔진 구조"로 경계를 다시 세워야 한다.

핵심은 다음 한 문장으로 정리된다.

`SQL 실행 엔진`과 `HTTP 요청 처리 서버`를 분리하고, 그 사이를 `thread-safe한 단일 엔진 API`로 연결해야 한다.

---

## 4. 이번 과제에서 채택하는 최종 방향

이번 문서는 아래 구조를 기준으로 리팩터링을 진행한다.

### 4.1 최상위 구조

전체 시스템은 아래 3계층으로 나눈다.

1. DB Engine
2. API Server
3. Concurrency Infrastructure

각 계층의 역할은 다음과 같다.

### 4.2 DB Engine

DB Engine은 아래 책임만 가진다.

- SQL 문자열 입력 받기
- tokenize / parse 수행
- statement 종류 판별
- 테이블 runtime 접근
- B+ 트리 인덱스 활용
- 실행 결과를 구조화된 형태로 반환

DB Engine은 아래를 몰라야 한다.

- socket
- HTTP header
- JSON body
- status code
- thread pool queue

### 4.3 API Server

API Server는 아래 책임만 가진다.

- 클라이언트 연결 수락
- HTTP 요청 읽기
- JSON body 파싱
- SQL 문자열 추출 또는 조립
- DB Engine 호출
- 실행 결과를 JSON 응답으로 직렬화
- 적절한 HTTP status code 반환

### 4.4 Concurrency Infrastructure

동시성 계층은 아래 책임을 가진다.

- bounded request queue 관리
- worker thread pool 운영
- shutdown 시퀀스 제어
- 요청 overload 시 fail-fast 처리

이렇게 나누면 각 레이어를 설명하기 쉽고, 테스트도 각각 나눠서 설계할 수 있다.

---

## 5. 권장 아키텍처

이번 과제 기준 권장 아키텍처는 아래와 같다.

```text
Client
  -> HTTP API Server
    -> Accept Loop
      -> Bounded Request Queue
        -> Worker Thread Pool
          -> engine_execute_sql()
            -> tokenizer
            -> parser
            -> executor
            -> table runtime
            -> B+ tree index
```

### 5.1 요청 처리 흐름

1. 클라이언트가 HTTP 요청을 보낸다.
2. 메인 서버 스레드는 `accept()` 후 요청을 queue에 넣는다.
3. worker thread가 queue에서 요청을 꺼낸다.
4. worker는 요청 body에서 SQL을 얻는다.
5. worker는 `engine_execute_sql()`을 호출한다.
6. 엔진은 필요한 lock을 획득한 뒤 SQL을 실행한다.
7. 엔진은 결과를 `QueryResult` 구조체로 반환한다.
8. worker는 이를 JSON으로 변환해 응답한다.
9. 응답 전송 후 소켓을 닫는다.

### 5.2 중요한 설계 원칙

- worker 하나가 하나의 요청을 끝까지 처리한다.
- 메인 스레드는 DB 엔진을 직접 호출하지 않는다.
- lock은 DB 엔진 내부에서만 관리한다.
- JSON 직렬화와 socket write는 DB lock 밖에서 수행한다.
- correctness가 성능보다 우선이다.

---

## 6. DB 엔진 리팩터링 방향

이번 과제에서 가장 중요한 리팩터링은 "기존 SQL 처리기를 서버에서 안전하게 부를 수 있는 형태로 바꾸는 것"이다.

### 6.1 단일 진입점 도입

서버는 tokenizer, parser, executor를 각각 알 필요가 없다.
서버는 아래 함수 하나만 호출하도록 구조를 고정한다.

```c
int engine_execute_sql(const char *sql, QueryResult *out_result);
void query_result_free(QueryResult *result);
```

이 함수 내부에서 아래를 수행한다.

1. tokenize
2. parse
3. statement type 판별
4. 필요한 table lock 획득
5. 실행
6. 결과 복사
7. lock 해제
8. 구조화된 결과 반환

### 6.2 executor 출력 제거

현재 executor가 직접 출력하는 구조는 API 서버에서 재사용하기 어렵다.
따라서 실행 결과는 반드시 구조체로 반환하도록 바꾼다.

예시:

```c
typedef enum {
    QUERY_RESULT_MESSAGE,
    QUERY_RESULT_TABLE
} QueryResultKind;

typedef struct {
    int success;
    QueryResultKind kind;
    char message[256];
    char error[256];
    char columns[MAX_COLUMNS][MAX_IDENTIFIER_LEN];
    int column_count;
    char ***rows;
    int row_count;
} QueryResult;
```

초기 구현에서는 최소 필드만 둔다.

- `success`
- `message`
- `error`
- `columns`
- `rows`

추가 메타데이터는 1차 구현이 끝난 뒤 붙인다.

### 6.3 tokenizer 전역 cache 제거

이번 과제는 학습용 최적화보다 안정적인 서버 구현이 더 중요하다.
따라서 tokenizer의 전역 cache는 제거하거나 서버 경로에서 비활성화한다.

이 선택의 장점은 분명하다.

- 전역 shared state 감소
- 별도 mutex 필요성 제거
- 동시성 reasoning 단순화
- 디버깅 난이도 감소

### 6.4 에러 처리 구조화

서버 응답을 위해, 엔진은 출력 대신 명시적인 결과와 에러 메시지를 제공해야 한다.

예시:

- tokenizer 실패 -> `result.error`
- parser 실패 -> `result.error`
- 존재하지 않는 테이블 -> `result.error`
- 지원하지 않는 SQL -> `result.error`

첫 단계에서는 완벽히 상세한 오류 메시지보다 일관된 오류 형식이 더 중요하다.

---

## 7. 동시성 모델

이번 과제의 중점 포인트 중 하나가 멀티 스레드 동시성 이슈이므로, 락 전략을 문서에서 명확히 고정한다.

### 7.1 락 단위

1차 구현은 `테이블 단위 rwlock`을 사용한다.

이 선택 이유는 다음과 같다.

- 기존 multi-table runtime 구조와 가장 잘 맞는다.
- 다른 테이블끼리는 자연스럽게 병렬 처리 가능하다.
- 같은 테이블에서도 `SELECT + SELECT` 병렬 허용이 가능하다.
- `INSERT` / `DELETE` / 구조 변경성 작업은 배타 처리할 수 있다.
- DB 전체 락보다 병렬성이 높고, 인덱스 단위 락보다 구현이 단순하다.

### 7.2 기대 동작

- 같은 테이블에 대한 `SELECT + SELECT`: 병렬 허용
- 같은 테이블에 대한 `SELECT + INSERT`: 상호 배제
- 같은 테이블에 대한 `INSERT + INSERT`: 상호 배제
- 다른 테이블 요청들: 서로 독립적으로 처리

### 7.3 권장 API

```c
int table_runtime_acquire_read(const char *table_name, TableRuntimeHandle *out_handle);
int table_runtime_acquire_write(const char *table_name, TableRuntimeHandle *out_handle);
void table_runtime_release(TableRuntimeHandle *handle);
```

### 7.4 read와 write의 registry 정책 분리

read 요청은 "없으면 실패"가 맞다.
write 요청은 "없으면 생성"이 맞다.

즉 아래처럼 분리하는 방향이 적합하다.

- `get_for_read()`: 없으면 실패
- `get_or_create_for_write()`: 없으면 생성

이렇게 해야 잘못된 조회 요청이 registry를 불필요하게 오염시키지 않는다.

### 7.5 lock 유지 범위

lock은 DB 데이터 접근과 결과 복사까지만 잡는다.

lock을 잡은 채로 아래 작업을 하면 안 된다.

- JSON 직렬화
- HTTP 응답 생성
- `send()` / `write()` 호출

그 이유는 단순하다.

- 느린 네트워크 때문에 DB가 불필요하게 막히면 안 된다.
- lock 보유 시간이 길어지면 병렬성 이점이 사라진다.
- deadlock과 starvation 분석이 어려워진다.

---

## 8. API 서버 설계

이번 과제는 "외부 클라이언트가 DBMS 기능을 사용할 수 있어야 한다"가 핵심이므로, 서버는 반드시 설명 가능하고 구현 가능한 최소 구조여야 한다.

### 8.1 서버 기본 정책

- worker thread 수: 8
- request queue capacity: 32
- queue full 시 즉시 실패
- keep-alive는 초기 구현에서 지원하지 않음
- 최소 HTTP/1.1 요청만 지원

### 8.2 권장 API 형태

이번 과제에서는 구현 속도와 기존 SQL 엔진 재사용성을 위해 `POST /query` 단일 endpoint를 기본안으로 채택한다.

요청 예시:

```http
POST /query HTTP/1.1
Content-Type: application/json
Content-Length: ...

{
  "sql": "SELECT * FROM users;"
}
```

이 방식을 기본안으로 두는 이유는 다음과 같다.

- 기존 SQL parser를 거의 그대로 재사용할 수 있다.
- 서버 라우팅 복잡도가 낮다.
- 데모와 기능 테스트를 빠르게 만들 수 있다.

기능별 endpoint가 꼭 필요하다면 이후 확장으로 둔다.

### 8.3 응답 형식

성공 예시:

```json
{
  "success": true,
  "message": "1 row inserted into users."
}
```

또는:

```json
{
  "success": true,
  "columns": ["id", "name", "age"],
  "rows": [
    ["1", "Alice", "30"]
  ]
}
```

실패 예시:

```json
{
  "success": false,
  "error": "Table 'users' not found."
}
```

### 8.4 HTTP status code 기준

- `200 OK`: 정상 실행
- `400 Bad Request`: 잘못된 JSON, SQL 누락, 파싱 실패
- `404 Not Found`: 잘못된 path
- `409 Conflict`: 필요 시 논리 충돌 표현
- `500 Internal Server Error`: 내부 예외
- `503 Service Unavailable`: queue full 또는 overload

---

## 9. 구현 범위 고정

과제를 제시간 안에 완성하려면 "이번 주에 반드시 할 것"과 "지금은 하지 않을 것"을 분리해야 한다.

### 9.1 이번 주 필수 범위

- `POST /query` API
- thread pool + bounded queue
- 구조화된 엔진 진입점
- table 단위 rwlock
- `INSERT`, `SELECT`, 필요 시 `DELETE`
- JSON 응답 직렬화
- 단위 테스트
- 동시성 테스트
- API 기능 테스트

### 9.2 이번 주 보류 범위

- 완전한 범용 HTTP parser
- chunked encoding
- keep-alive connection reuse
- 인덱스 단위 세밀한 락
- persistence와 memory runtime 완전 재통합
- metrics / tracing / admin endpoint
- tokenizer cache 최적화 부활

### 9.3 persistence 판단

현재 코드베이스에는 `storage.c`가 존재하지만, 이번 과제의 핵심 리스크는 persistence보다 동시성 서버 구조다.

따라서 기본 방침은 다음과 같다.

- 1차 목표는 메모리 기반 DB 엔진 + API 서버 완성
- persistence는 시간이 남거나 평가 기준상 꼭 필요할 때만 2차 범위로 추가

이 판단은 과제 성공 확률을 높이기 위한 전략적 축소다.

---

## 10. 단계별 구현 계획

이번 리팩터링은 아래 순서로 진행하는 것이 가장 안전하다.

### 10.1 1단계: 엔진 경계 정리

대상:

- `src/executor.c`
- `src/executor.h`
- 신규 `src/engine.c`
- 신규 `src/engine.h`

작업:

- executor 출력 제거
- `QueryResult` 도입
- `engine_execute_sql()` 단일 진입점 도입
- CLI가 새 엔진 API를 호출하도록 변경

완료 기준:

- SQL 실행 결과가 구조체로 반환된다.
- CLI와 서버가 같은 엔진을 공유할 수 있다.

### 10.2 2단계: tokenizer 정리

대상:

- `src/tokenizer.c`
- `src/tokenizer.h`
- 관련 테스트

작업:

- 전역 cache 제거 또는 비활성화
- 함수 내부 상태 의존성 최소화

완료 기준:

- tokenizer가 멀티 스레드 환경에서 숨은 공유 상태를 갖지 않는다.

### 10.3 3단계: table runtime 동시성 리팩터링

대상:

- `src/table_runtime.c`
- `src/table_runtime.h`
- 관련 테스트

작업:

- `pthread_mutex_t`를 `pthread_rwlock_t`로 교체
- read/write acquire API 분리
- read miss와 write miss 동작 분리

완료 기준:

- 같은 테이블 read/read 병렬 허용
- write는 배타 처리
- 잘못된 read 요청이 빈 엔트리를 남기지 않는다.

### 10.4 4단계: thread pool과 queue 구현

대상:

- 신규 `src/server_queue.c`
- 신규 `src/server_queue.h`
- 신규 `src/thread_pool.c`
- 신규 `src/thread_pool.h`

작업:

- bounded queue 구현
- mutex + condition variable 기반 pop/push
- worker thread 생성 및 join

완료 기준:

- 요청을 안전하게 큐잉하고 worker가 소모할 수 있다.
- queue full 시 즉시 실패 가능하다.

### 10.5 5단계: HTTP 서버 구현

대상:

- 신규 `src/api_server.c`
- 신규 `src/api_server.h`
- 필요 시 `src/http_parser.c`

작업:

- socket 생성
- `bind`, `listen`, `accept`
- 최소 HTTP request 파싱
- JSON body에서 SQL 추출
- 엔진 호출 후 JSON 응답 생성

완료 기준:

- 외부 클라이언트가 실제로 SQL 요청을 보내고 결과를 받을 수 있다.

### 10.6 6단계: 통합 테스트와 데모 시나리오 정리

작업:

- 단위 테스트 보강
- 동시성 테스트 추가
- 실제 HTTP 요청 기반 기능 테스트 추가
- 발표용 시나리오와 샘플 쿼리 정리

완료 기준:

- 발표 직전 "무엇을 어떻게 검증했는지" 설명 가능하다.

---

## 11. 테스트 전략

이번 과제는 멀티 스레드 서버이므로 테스트 전략도 구조적으로 가져가야 한다.

### 11.1 단위 테스트

반드시 필요한 단위 테스트:

1. tokenizer / parser 기본 SQL 처리
2. `QueryResult` 생성과 해제
3. table runtime read/write lock 동작
4. 존재하지 않는 테이블 read 처리
5. insert 후 select 결과 일관성

### 11.2 동시성 테스트

반드시 필요한 동시성 테스트:

1. 같은 테이블 `SELECT + SELECT`
2. 같은 테이블 `SELECT + INSERT`
3. 같은 테이블 `INSERT + INSERT`
4. 다른 테이블 간 병렬 접근
5. 잘못된 read 요청 반복 시 registry 오염 여부

### 11.3 API 기능 테스트

반드시 필요한 API 테스트:

1. `POST /query`로 insert 성공
2. `POST /query`로 select 성공
3. 잘못된 SQL 요청 실패
4. 없는 path 요청 실패
5. overload 상황에서 503 반환

### 11.4 발표용 검증 포인트

발표나 시연에서는 아래가 잘 보이면 좋다.

- 같은 서버에 여러 요청을 동시에 보내도 정상 동작
- insert 후 select 결과가 일관됨
- 잘못된 요청도 서버가 죽지 않고 에러 응답
- worker pool 구조 설명 가능

---

## 12. 차별화 포인트 제안

과제 요구사항에도 "다른 팀과 차별점을 둘 수 있는 추가 구현 요소"를 고민하라고 되어 있으므로, 기본 구현이 완료된 뒤 아래 중 하나를 선택적으로 추가할 수 있다.

### 12.1 추천 차별화 요소

- 동시 요청 benchmark 도구 추가
- 간단한 `/health` 또는 `/stats` endpoint 추가
- query execution result에 `index_used` 같은 설명 정보 추가
- 서버 overload 시 명시적인 JSON 에러 정책 제공
- 발표용 seed data 및 시나리오 자동 실행 스크립트 추가

### 12.2 가장 현실적인 추천

가장 구현 대비 효과가 좋은 차별화 요소는 아래 두 가지다.

1. 간단한 benchmark 클라이언트 제공
2. 응답에 "인덱스 사용 여부" 또는 "처리 결과 요약" 포함

이 두 가지는 발표에서 구조적 완성도를 보여주기 쉽고, 구현 난이도도 상대적으로 통제 가능하다.

---

## 13. 최종 결론

이번 과제를 원활하게 수행하려면 기존 구조를 부분 수정하는 수준을 넘어서, "CLI 중심 SQL 처리기"를 "concurrent API 서버가 호출할 수 있는 DB 엔진"으로 재정렬해야 한다.

이번 문서에서 채택한 최종 방향은 아래와 같다.

- 엔진과 HTTP 서버를 명확히 분리한다.
- 서버는 `POST /query` 중심의 최소 구조로 시작한다.
- 스레드 풀과 bounded queue를 사용한다.
- 테이블 단위 `pthread_rwlock_t`로 동시성 모델을 단순하고 안전하게 설계한다.
- executor는 출력 대신 `QueryResult`를 반환한다.
- tokenizer의 전역 mutable state는 제거한다.
- correctness와 설명 가능성을 성능 최적화보다 우선한다.

이 방향은 현재 코드 자산을 최대한 재사용하면서도, 이번 주 과제를 실제로 완성할 가능성이 가장 높은 구조다.
