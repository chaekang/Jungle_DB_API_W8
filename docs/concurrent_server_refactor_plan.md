# Concurrent Server Refactor Plan

## 1. 목적

이 문서는 현재 `main` 브랜치의 multi-table 변경이 무엇을 해결했고 무엇을 아직 해결하지 못했는지 정리한 뒤,
현재 DB 구조를 concurrent한 API 서버에 연결할 수 있도록 바꾸는 상세 계획을 기록한다.

이 문서의 목표는 두 가지다.

1. 팀 문서에서 이미 정한 설계 방향을 최대한 유지한다.
2. 서버 코드와 DB 리팩터링 모두에서 가독성과 구현 단순성을 최우선으로 둔다.

---

## 2. 현재 `main` 브랜치 검토 결과

### 2.1 multi-table 이슈는 해결됐다

현재 `main` 의 multi-table 변경은 원래 문제였던 "다른 테이블 접근이 기존 active runtime을 날려버리는 문제"를 해결했다.

현재 구조는:

- 단일 `table_runtime_active` 싱글턴이 아니라
- `table_name -> TableRuntimeEntry` 연결 리스트 registry를 두고
- 테이블별로 독립된 `TableRuntime` 을 유지한다.

실제 코드 기준으로 보면:

- `src/table_runtime.c` 는 `TableRuntimeEntry` registry를 도입했다.
- `src/executor.c` 는 `table_runtime_acquire()` / `table_runtime_release()` 를 통해 테이블별 상태를 접근한다.
- `tests/test_table_runtime.c` 와 `tests/test_executor.c` 는 서로 다른 테이블 상태가 독립적으로 유지되는지를 검증한다.
- `tests/test_table_runtime_concurrency.c` 는 서로 다른 두 테이블에 대해 병렬 insert를 돌려 cross-table isolation이 유지되는지를 검증한다.

즉, "users 처리 중 orders 요청이 와서 users 메모리 상태가 사라진다"는 종류의 문제는 지금 구조에서 더 이상 발생하지 않는다.

### 2.2 하지만 이것만으로 서버를 붙일 수는 없다

현재 변경은 "multi-table isolation"까지는 해결했지만, 팀 문서가 목표로 한 concurrent server용 DB 엔진으로는 아직 부족하다.

남아 있는 핵심 문제는 아래와 같다.

#### 문제 1. 같은 테이블에 대한 읽기/읽기 병렬이 아직 안 된다

현재 테이블 엔트리 락은 `pthread_mutex_t` 이다.

- 다른 테이블끼리는 병렬 가능
- 같은 테이블은 `SELECT` 와 `SELECT` 도 서로 직렬화

팀 문서의 목표는:

- 읽기끼리 병렬 허용
- 쓰기는 한 번에 하나
- `pthread_rwlock` 사용

따라서 현재 구현은 multi-table 문제는 해결했지만, 합의된 concurrency 모델은 아직 만족하지 못한다.

#### 문제 2. tokenizer 캐시가 전역 mutable 상태다

현재 `src/tokenizer.c` 는 전역 linked list 기반 soft parser cache를 가진다.

- `tokenizer_cache_head`
- `tokenizer_cache_entry_count`
- `tokenizer_cache_hit_count`

이 구조는 mutex 없이 concurrent access 하면 안전하지 않다.

즉 서버 worker 여러 개가 동시에 `tokenizer_tokenize()` 를 호출하면:

- cache lookup
- cache entry move-to-front
- cache insert
- cache eviction

가 경쟁 상태가 된다.

#### 문제 3. executor가 아직 CLI 출력에 묶여 있다

현재 executor는 결과를 구조체로 반환하지 않고 바로 출력한다.

- `INSERT` 성공 시 `printf`
- `SELECT` 결과 표 출력
- row count 출력
- 오류는 `stderr` 출력

이 구조는 REPL에는 편하지만, 서버에서는 불편하다.

서버는 출력 문자열이 아니라 구조화된 결과가 필요하다.

- success
- rows
- error
- optional metadata

즉 서버를 붙이려면 DB 엔진이 `stdout/stderr` 대신 결과 구조체를 반환해야 한다.

#### 문제 4. read miss에도 빈 테이블 엔트리를 만들고 있다

현재 `table_runtime_acquire()` 는 테이블이 없으면 바로 새 엔트리를 만든다.

이 동작은 `INSERT` 에서는 자연스럽지만, `SELECT` 에서는 불필요한 registry 오염을 만든다.

예를 들어 서버에 임의의 잘못된 테이블 이름 요청이 계속 들어오면:

- 실패한 read 요청인데도
- 빈 `TableRuntimeEntry` 가 계속 registry에 남는다

이건 CLI에서는 큰 문제가 아닐 수 있지만, 서버에서는 좋지 않다.

#### 문제 5. cleanup은 "모든 worker 종료 후"라는 강한 전제를 가진다

현재 cleanup은 registry를 분리한 뒤 entry를 순회하며 free 한다.
이 함수는 안전하게 쓰려면 반드시:

1. accept 중단
2. worker 종료
3. 그 다음 cleanup

순서를 지켜야 한다.

즉 서버 shutdown 설계에서 이 전제를 분명히 문서화해야 한다.

---

## 3. 팀 문서에서 확정된 설계와 아직 미확정인 설계

## 3.1 문서에서 비교적 확정된 것

`goal.md` 와 `mini_dbms_api_server_design_questions.md` 기준으로 아래 항목은 거의 확정으로 본다.

- 구현 목표는 C 기반 API 서버다.
- 스레드 풀을 사용한다.
- worker 수는 8로 시작한다.
- bounded queue는 32로 시작한다.
- queue full은 fail-fast 한다.
- worker 하나가 parse -> execute -> respond 전 과정을 끝까지 담당한다.
- DB 엔진은 HTTP를 몰라야 한다.
- correctness를 성능보다 우선한다.
- 읽기끼리 병렬 허용, 쓰기는 배타적으로 처리한다.
- 락 primitive는 `pthread_rwlock_t` 방향을 선호한다.

## 3.2 문서에서 아직 흔들리는 것

아래는 문서 안에서도 아직 완전히 정리되지 않았다.

### API shape

문서 초반에는 `/insert`, `/select` 같은 기능별 endpoint를 선호한다.
하지만 후반 "현실적인 MVP 방향 예시"는 `POST /query` 하나를 든다.

즉 API shape은 아직 완전한 합의라고 보기 어렵다.

### 락 단위

문서에는:

- DB 전체
- 테이블 단위
- 인덱스 단위

가 모두 후보로 등장한다.
후반 예시는 DB 전체 `pthread_rwlock_t` 를 들지만, 중간 질문에서는 테이블 단위와 인덱스 단위를 계속 비교 중이다.

### persistence

문서 후반 예시는 "데이터 파일 저장 + 시작 시 인덱스 rebuild" 를 들지만,
현재 엔진의 실제 executor 경로는 메모리 runtime만 사용하고 `storage.c` 와는 연결되어 있지 않다.

즉 persistence는 현재 코드 기준으로는 아직 미구현이며, 서버 MVP에 반드시 포함해야 하는지 다시 정해야 한다.

---

## 4. 이 문서에서 채택하는 기본 방향

문서의 충돌을 정리한 뒤, 구현 복잡도와 가독성을 고려해 아래 방향을 채택한다.

### 4.1 DB 구조 방향

현재 multi-table registry는 유지한다.

이유:

- 이미 correct한 방향으로 리팩터링이 들어갔다.
- 지금 와서 다시 single active runtime으로 되돌릴 이유가 없다.
- 이후 per-table locking과 테이블별 상태 유지에 가장 잘 맞는다.

### 4.2 락 단위

1차 구현은 **테이블 단위 `pthread_rwlock_t`** 로 간다.

이유:

- current multi-table 구조를 가장 자연스럽게 확장한다.
- 같은 테이블 `SELECT` / `SELECT` 병렬 허용이 가능하다.
- 다른 테이블은 원래대로 독립 병렬 처리 가능하다.
- DB 전체 rwlock보다 병렬성은 낫고, 인덱스 단위 lock보다 훨씬 단순하다.

즉 "DB 전체 rwlock" 은 더 단순한 fallback이지만,
이미 multi-table 구조가 들어온 지금은 테이블 단위 rwlock이 더 일관된 선택이다.

### 4.3 tokenizer 처리

server path에서는 tokenizer cache를 **제거하거나 비활성화** 하는 쪽을 우선 추천한다.

이유:

- cache는 correctness에 필요한 기능이 아니다.
- 현재 팀 목표는 성능보다 correctness와 구현 단순성이다.
- tokenizer cache를 살리려면 별도 mutex가 필요하고 코드량이 늘어난다.
- 서버 MVP에서 tokenizer cache의 체감 성능 이익은 작다.

따라서 이 문서에서는 "cache 제거"를 기본안으로 본다.

### 4.4 엔진/HTTP 경계

DB 엔진은 SQL 문자열을 받아 결과 구조체를 반환하는 함수만 제공한다.
HTTP 서버는 그 위에서:

- JSON body parse
- SQL 문자열 생성 또는 추출
- DB 호출
- JSON 응답 직렬화

만 담당한다.

즉 DB 엔진은 socket, HTTP header, Content-Length, status code를 몰라야 한다.

### 4.5 코드 스타일 원칙

서버 코드와 DB 리팩터링 모두에서 아래 원칙을 강제한다.

- 미래 확장용 추상화는 넣지 않는다.
- 지금 당장 쓰지 않을 옵션/전략 객체/플러그인 구조는 만들지 않는다.
- 빠른 코드보다 읽기 쉬운 코드를 우선한다.
- 한 단계에서 한 문제만 해결한다.
- 가능하면 함수 이름만 봐도 흐름이 읽히게 만든다.
- 성능 최적화는 correctness 이후에만 한다.

---

## 5. 현재 DB 구조 상세 분석

현재 엔진 흐름은 아래와 같다.

1. `main.c`
   - SQL 문자열 입력
   - `tokenizer_tokenize()`
   - `parser_parse()`
   - `executor_execute()`

2. `tokenizer.c`
   - SQL 문자열을 토큰 배열로 변환
   - 현재는 전역 soft parser cache 포함

3. `parser.c`
   - 토큰 배열을 `SqlStatement` 로 변환
   - 지원 문법은 `INSERT`, `SELECT`, `DELETE(parse only)`

4. `executor.c`
   - statement type 분기
   - memory runtime insert/select 수행
   - 결과를 직접 출력

5. `table_runtime.c`
   - 테이블별 메모리 row 저장
   - auto increment id
   - B+Tree 인덱스 유지
   - linear scan
   - 현재 multi-table registry 포함

6. `storage.c`
   - CSV 기반 파일 저장/조회/삭제 계층
   - 하지만 현재 executor 경로에서는 사용하지 않음

핵심 해석은 이렇다.

- parser는 비교적 독립적이다.
- table runtime도 low-level 데이터 구조로는 재사용 가능하다.
- 가장 큰 서버 연결 장애물은 executor 출력 방식과 tokenizer global state다.
- persistence는 "있지만 현재 실행 경로에 연결되지 않은 보조 계층" 상태다.

---

## 6. concurrent server를 붙이기 위해 DB가 바뀌어야 하는 지점

## 6.1 변경 1: `TableRuntimeEntry.lock` 을 `pthread_rwlock_t` 로 교체

현재:

```c
typedef struct TableRuntimeEntry {
    TableRuntime table;
    pthread_mutex_t lock;
    struct TableRuntimeEntry *next;
} TableRuntimeEntry;
```

목표:

```c
typedef struct TableRuntimeEntry {
    TableRuntime table;
    pthread_rwlock_t lock;
    struct TableRuntimeEntry *next;
} TableRuntimeEntry;
```

그리고 API도 read/write를 나눠야 한다.

예시:

```c
int table_runtime_acquire_read(const char *table_name, TableRuntimeHandle *out_handle);
int table_runtime_acquire_write(const char *table_name, TableRuntimeHandle *out_handle);
void table_runtime_release(TableRuntimeHandle *handle);
```

이 변경의 의미:

- 같은 테이블 `SELECT` / `SELECT` 병렬 허용
- `INSERT` 는 write lock
- `SELECT` 와 `INSERT` 는 상호 배제
- 다른 테이블은 기존대로 독립

## 6.2 변경 2: read path와 write path에서 registry 동작을 분리

현재 `table_runtime_acquire()` 는 "없으면 무조건 생성"이다.

서버용으로는 아래처럼 나누는 편이 더 낫다.

- `table_runtime_get_for_read()`
  - 없으면 실패
  - 빈 엔트리 생성 안 함

- `table_runtime_get_or_create_for_write()`
  - 없으면 생성
  - `INSERT` 첫 요청에서 스키마 초기화 가능

이렇게 해야:

- 잘못된 `SELECT` 요청이 registry를 오염시키지 않는다.
- 메모리 사용량을 더 예측 가능하게 유지할 수 있다.

## 6.3 변경 3: tokenizer cache 제거

현재 tokenizer cache는 서버 MVP에 비해 얻는 것보다 잃는 것이 많다.

제거 시 장점:

- 전역 mutable linked list 제거
- 별도 tokenizer mutex 제거
- worker 간 hidden shared state 감소
- 코드량 감소

즉 concurrent server를 붙이는 관점에서는
"cache를 thread-safe하게 고친다"보다 "cache를 없앤다"가 더 읽기 쉽고 더 안전하다.

## 6.4 변경 4: executor를 구조화된 결과 반환형으로 바꾸기

현재 executor는 CLI 출력과 DB 실행이 결합돼 있다.
이 구조는 서버에 바로 쓰기 어렵다.

따라서 새 결과 타입을 둔다.

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
    int index_used;
} QueryResult;
```

중요한 점은 "필요한 최소 필드만 둔다"는 것이다.

처음부터:

- execution_time
- metrics
- tracing id
- warnings array

같은 확장 필드는 넣지 않는다.

최소한 아래만 있으면 서버 응답은 만들 수 있다.

- success
- message or error
- rows
- columns

## 6.5 변경 5: `engine_execute_sql()` 하나로 엔진 경계를 고정

서버가 직접 tokenizer, parser, executor를 각각 호출하지 않게 만든다.

새 public entrypoint 예시:

```c
int engine_execute_sql(const char *sql, QueryResult *out_result);
void query_result_free(QueryResult *result);
```

이 함수 내부에서:

1. tokenize
2. parse
3. statement type 판별
4. 필요한 read/write lock 획득
5. executor-style 실행
6. 결과 구조체 반환

까지 끝낸다.

이 구조의 장점:

- CLI와 서버가 같은 엔진 entrypoint를 공유
- HTTP 계층이 DB 내부를 몰라도 됨
- 락 획득/해제 위치가 한 군데로 모임

## 6.6 변경 6: lock은 "결과 복사 완료 직후"에 풀어야 한다

이건 매우 중요하다.

서버에서는 worker가 결국:

- DB 실행
- JSON 직렬화
- socket write

를 하게 되는데, DB lock을 socket write까지 잡고 있으면 안 된다.

따라서 read/write lock은:

- runtime에서 필요한 row를 읽고
- 결과를 `QueryResult` 로 복사한 뒤
- 즉시 release

해야 한다.

그 뒤:

- JSON 문자열 만들기
- HTTP 응답 쓰기

는 lock 밖에서 해야 한다.

즉 현재 executor가 표 출력까지 lock 안에서 하는 구조는 서버용으로 그대로 쓰면 안 된다.

## 6.7 변경 7: error reporting을 stderr 출력에서 result/error 문자열로 이동

서버 응답에는 구조화된 에러가 필요하다.

따라서 적어도 high-level 경로는 다음을 만족해야 한다.

- tokenizer 실패 -> `result.error`
- parser 실패 -> `result.error`
- missing table -> `result.error`
- unsupported statement -> `result.error`

이때 모든 low-level helper까지 완벽한 error plumbing을 처음부터 넣으려 하지 않는다.

권장 전략:

1. high-level public 함수부터 error buffer 추가
2. 자주 쓰는 failure path만 명시적 메시지 제공
3. 나머지는 generic error로 시작

즉 첫 서버 구현에서는 "짧고 일관된 에러"가 "완벽한 진단 메시지"보다 낫다.

---

## 7. persistence에 대한 판단

현재 codebase에는 `storage.c` 가 있지만 executor는 memory runtime만 사용한다.

이 상태에서 persistence까지 한 번에 붙이면:

- file lock
- memory runtime lock
- index rebuild
- write ordering
- startup loading

을 동시에 정리해야 한다.

이건 오늘 범위를 크게 키운다.

따라서 이 문서의 기본 권고는:

### 서버 1차 구현은 memory runtime 기준으로 끝낸다

이유:

- 현재 실제 실행 경로가 그쪽이다.
- 코드 양을 가장 적게 유지할 수 있다.
- concurrency reasoning이 훨씬 단순하다.

만약 팀이 persistence를 오늘의 필수 범위로 본다면,
그건 "서버 붙이기 전에" 별도 결정을 다시 해야 한다.

즉 persistence는 현재 문서 기준으로 **보류 항목**이다.

---

## 8. 서버 구현 계획

## 8.1 HTTP 계층은 얇게 둔다

서버는 아래 역할만 한다.

- socket listen
- accept
- bounded queue push
- worker wakeup
- request parse
- engine call
- JSON 응답

DB 엔진 쪽 로직은 넣지 않는다.

## 8.2 thread pool 구조

문서 합의대로 아래 값을 사용한다.

- worker thread: 8
- queue capacity: 32
- queue full: 즉시 실패

main thread 역할:

- accept
- queue push
- queue full이면 즉시 overload 응답

worker 역할:

- request parse
- SQL 문자열 준비
- `engine_execute_sql()`
- JSON 응답 직렬화
- socket write
- close

## 8.3 API shape는 engine 계획과 분리해서 본다

문서가 `/insert` / `/select` 와 `/query` 사이에서 아직 흔들리므로,
DB 쪽 계획은 특정 HTTP shape에 종속되지 않게 둔다.

즉 엔진은 항상:

```text
SQL string -> QueryResult
```

형태만 제공한다.

그 위에 서버는 두 가지 중 하나를 선택할 수 있다.

### 옵션 A. `POST /query`

장점:

- 서버 코드 가장 짧음
- 현재 SQL processor 재사용성이 높음
- 구현 속도가 빠름

단점:

- 문서 초반의 "기능별 endpoint" 선호와 충돌

### 옵션 B. `/insert`, `/select` 같은 기능별 endpoint

장점:

- 외부 입력 통제 쉬움
- 문서 초반 의도와 더 잘 맞음

단점:

- 서버 라우팅과 요청 검증 코드가 늘어남
- 내부적으로 결국 SQL 문자열 조립 로직이 필요

이 문서의 권고는 다음과 같다.

- DB 리팩터링은 두 옵션 모두 공통으로 쓰게 만든다.
- HTTP shape는 팀이 마지막에 고른다.
- 코드량 최우선이면 `POST /query`
- 외부 입력 통제 우선이면 기능별 endpoint

## 8.4 응답 포맷

성공:

```json
{
  "success": true,
  "message": "1 row inserted into users."
}
```

또는

```json
{
  "success": true,
  "columns": ["id", "name", "age"],
  "rows": [
    ["1", "Alice", "30"]
  ]
}
```

실패:

```json
{
  "success": false,
  "error": "Table 'users' not found."
}
```

초기 구현에서는 이 정도면 충분하다.

- `execution_time_ms`
- `index_used`
- `/metrics`

는 core 흐름이 완성된 뒤 넣는 것이 낫다.

---

## 9. DB 리팩터링 상세 순서

## 9.1 1단계: tokenizer cache 제거

대상:

- `src/tokenizer.c`
- `src/tokenizer.h`
- 관련 테스트

작업:

- cache entry 자료구조 삭제
- lookup/store/evict 관련 함수 삭제
- `tokenizer_tokenize()` 를 순수 함수에 가깝게 단순화

완료 기준:

- `tokenizer_tokenize()` 는 입력 문자열과 지역 메모리만 사용
- concurrent server path에서 tokenizer 전역 lock이 필요 없음

## 9.2 2단계: table runtime lock을 rwlock으로 교체

대상:

- `src/table_runtime.c`
- `src/table_runtime.h`
- 관련 테스트

작업:

- `pthread_mutex_t` -> `pthread_rwlock_t`
- read acquire / write acquire 분리
- read miss에서는 create하지 않는 API 추가
- write path에서만 create 허용

권장 public API 예시:

```c
int table_runtime_acquire_read(const char *table_name, TableRuntimeHandle *out_handle);
int table_runtime_acquire_write(const char *table_name, TableRuntimeHandle *out_handle);
TableRuntime *table_runtime_handle_table(TableRuntimeHandle *handle);
void table_runtime_release(TableRuntimeHandle *handle);
```

완료 기준:

- 같은 테이블 read/read 병렬 허용
- write는 배타적
- invalid read가 registry를 부풀리지 않음

## 9.3 3단계: executor 출력 제거, 결과 구조체 도입

대상:

- `src/executor.c`
- `src/executor.h`
- 신규 `src/query_result.h` 또는 `src/engine.h`
- CLI adapter

작업:

- 표 출력 제거
- `printf`, `puts`, `putchar` 제거
- `QueryResult` 채우는 방식으로 전환
- select는 결과 row를 복사해서 반환
- insert는 message 문자열 반환

완료 기준:

- executor 또는 engine이 `stdout` 에 의존하지 않음
- CLI와 서버가 같은 결과 구조체를 사용

## 9.4 4단계: `engine_execute_sql()` 도입

대상:

- 신규 `src/engine.c`
- 신규 `src/engine.h`
- `src/main.c`

작업:

- tokenize
- parse
- read/write lock 선택
- execute
- `QueryResult` 반환

를 한 entrypoint에 모은다.

CLI `main.c` 는 이 함수를 호출한 뒤 기존 REPL 출력만 담당하게 바꾼다.

완료 기준:

- DB 엔진과 UI/HTTP 경계가 명확해짐

## 9.5 5단계: DB concurrency 테스트 추가

반드시 필요한 테스트:

1. 같은 테이블 `SELECT + SELECT`
   - 둘 다 성공
   - deadlock 없음

2. 같은 테이블 `SELECT + INSERT`
   - 결과 일관성 유지
   - crash 없음

3. 같은 테이블 `INSERT + INSERT`
   - id 중복/row 손상 없음

4. 다른 테이블 `SELECT + INSERT`
   - 서로 독립 유지

5. missing table read flood
   - registry가 불필요하게 커지지 않음

---

## 10. 서버 구현 상세 순서

## 10.1 1단계: thread-safe queue

작업:

- 요청 객체 정의
- mutex + condition variable 기반 bounded queue 구현
- push/pop API 구현

요청 객체는 최소 필드만 둔다.

- client fd
- request buffer

그 이상은 넣지 않는다.

## 10.2 2단계: worker pool

작업:

- worker 8개 생성
- 각 worker는 무한 루프에서 queue pop
- 요청 처리 후 fd close

## 10.3 3단계: minimal HTTP parser

초기 구현은 아래만 지원하면 된다.

- `POST`
- `Content-Length`
- JSON body

지원하지 않을 것:

- chunked encoding
- keep-alive 재사용
- multipart
- streaming

즉 요청 파서는 "과제 데모용 최소 HTTP" 로 둔다.

## 10.4 4단계: DB 연결

worker에서:

1. JSON parse
2. SQL 문자열 준비
3. `engine_execute_sql()`
4. result -> JSON 변환
5. HTTP 200/400/404/409/500/503 중 하나로 응답

## 10.5 5단계: overload 및 shutdown

queue full:

- 즉시 503
- queue에 넣지 않음

shutdown:

1. listen socket close
2. accept loop 종료
3. worker join
4. engine cleanup

순서로 고정한다.

---

## 11. 구현 이후의 선택적 개선 항목

아래는 "지금 당장" 하지 않는다.

- index 단위 lock
- file persistence 재통합
- tokenizer cache 부활
- generic router
- non-blocking I/O
- metrics endpoint
- benchmark-driven lock refinement

즉 처음에는 설명 가능한 작은 구조를 먼저 만든다.

---

## 12. 최종 권고

현재 `main` 의 multi-table 변경은 **문제를 올바른 방향으로 해결했다.**
하지만 concurrent server용 DB 엔진으로 가려면 아래가 추가로 필요하다.

1. tokenizer cache 제거
2. per-table `pthread_rwlock_t`
3. read path / write path 분리
4. executor의 출력 제거
5. `engine_execute_sql()` 도입
6. DB lock을 네트워크 응답 전에 해제하는 구조

이 순서를 지키면:

- 문서가 요구한 read/read 병렬성과 write 배타성을 맞출 수 있고
- 현재 multi-table 리팩터링을 버리지 않으면서
- 코드 양도 비교적 작게 유지할 수 있다.

가장 중요한 원칙은 아래 한 줄이다.

> **DB 엔진은 구조화된 결과만 반환하고, HTTP 서버는 그 결과를 얇게 감싸기만 하도록 유지한다.**
