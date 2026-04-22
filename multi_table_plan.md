# Multi-Table Runtime Refactor Plan

## 1. 왜 이 계획이 필요한가

`goal.md`의 핵심 요구사항은 "스레드 풀을 구성하고, 요청이 들어올 때마다 스레드를 할당하여 SQL 요청을 병렬로 처리"하는 것이다.  
그런데 현재 엔진은 병렬 처리 이전에 런타임 상태 모델부터 이 요구사항과 충돌한다.

- `src/table_runtime.c:11` 에는 `static TableRuntime table_runtime_active;` 하나만 있다.
- `src/table_runtime.c:239-261` 의 `table_get_or_load()`는 다른 테이블 이름이 오면 기존 런타임을 `table_free()`로 통째로 비우고 새 테이블 이름을 넣는다.
- `tests/test_table_runtime.c:117-123` 도 이 동작을 정상으로 간주하고 "테이블 전환 시 rows가 초기화된다"를 검증한다.
- `src/executor.c:400`, `src/executor.c:430` 은 모든 `INSERT`, `SELECT`가 이 단일 active runtime에 의존한다.

즉 지금 구조에서는:

- `users` 테이블을 처리하던 중 `orders` 요청이 오면 `users` 런타임이 해제될 수 있다.
- 다른 테이블끼리 상태를 분리해서 유지할 수 없다.
- "테이블 단위 락"이나 "인덱스 단위 락"을 논하기 전에, 애초에 테이블이 독립된 객체로 공존하지 못한다.
- 요청이 섞이면 병렬성이 없는 수준이 아니라 논리적 데이터 손실까지 발생할 수 있다.

이번 계획은 이 문제만 해결한다. 목표는 "다른 테이블끼리 독립된 런타임을 유지하고, 서로 다른 테이블 요청은 동시에 진행될 수 있게 만드는 것"이다.

## 2. 이번 변경의 목표

이번 리팩토링이 끝나면 아래 상태를 만족해야 한다.

- 테이블마다 별도의 `TableRuntime` 인스턴스가 존재한다.
- `users` 와 `orders` 를 번갈아 접근해도 서로의 `rows`, `next_id`, `id_index_root`, `loaded` 상태가 유지된다.
- 같은 프로세스 안에서 여러 worker가 다른 테이블을 동시에 처리할 수 있다.
- 같은 테이블에 대해서는 우선 직렬화해 correctness를 보장한다.
- 현재 `table_insert_row()`, `table_linear_scan_by_field()`, `table_get_row_by_slot()` 같은 저수준 로직은 최대한 재사용한다.
- `benchmark.c`처럼 로컬 `TableRuntime` 값을 직접 다루는 경로는 계속 동작한다.

## 3. 이번 변경의 비목표

범위를 통제하기 위해 아래 항목은 이번 계획에 포함하지 않는다.

- 같은 테이블에서 `SELECT`끼리 read/read 병렬 허용
- 같은 테이블에 대한 read/write 병행 제어 고도화
- `pthread_rwlock_t` 도입
- `executor`를 `QueryResult` 반환형으로 바꾸는 리팩토링
- `storage.c` 기반 영속화 경로 재통합
- `DELETE`, `UPDATE`, `CREATE TABLE` 같은 기능 확장
- 런타임 eviction/LRU/cache size 제한
- API 서버 구현 자체

이번 단계는 어디까지나 "전역 단일 active runtime 제거"가 목적이다.

## 4. 현재 코드의 정확한 문제 정리

### 4.1 런타임 저장 방식 자체가 싱글턴이다

현재 `table_runtime.c`는 모듈 전역 정적 변수 두 개로 상태를 유지한다.

- `table_runtime_active`
- `table_runtime_has_active`

이 구조의 의미는 "엔진 전체에 메모리 테이블은 하나만 존재한다"는 것이다.

### 4.2 테이블 전환이 곧 데이터 삭제다

`table_get_or_load("A")` 후 `table_get_or_load("B")`를 호출하면:

1. 기존 `A` 런타임 전체를 `table_free()` 한다.
2. `table_runtime_active.table_name` 에 `B`를 기록한다.
3. 결과적으로 `A`의 `rows`, `next_id`, `id_index_root`가 사라진다.

이 동작은 "테이블 선택"이 아니라 "전역 메모리 교체"다.

### 4.3 executor가 이 전제에 깊게 묶여 있다

`executor_execute_insert()` 와 `executor_execute_select()` 는 모두 `table_get_or_load(stmt->table_name)` 로 시작한다.  
즉 executor는 현재 "테이블별 객체"가 아니라 "active runtime 하나"에 기대고 있다.

### 4.4 테스트가 잘못된 계약을 고정하고 있다

`tests/test_table_runtime.c` 는 현재 잘못된 동작을 회귀 기준으로 삼고 있다.

- 지금 테스트는 다른 테이블을 열면 이전 테이블 row가 사라지는 것을 기대한다.
- 우리가 원하는 미래 계약은 반대다.
- 따라서 리팩토링 전 첫 번째 작업 중 하나는 테스트 계약부터 새 요구사항에 맞게 뒤집는 것이다.

## 5. 목표 상태의 설계 원칙

이번 변경은 아래 5개 원칙을 따른다.

1. `TableRuntime` 자체는 유지한다.  
   row 저장, auto increment id, B+Tree 갱신, 선형 탐색 로직은 이미 모듈화돼 있으므로 버리지 않는다.

2. 전역 싱글턴 대신 "테이블 이름 -> runtime entry" 레지스트리를 도입한다.  
   핵심 문제는 `TableRuntime` 자료구조가 아니라, 그것을 한 개만 보관하는 현재 상위 레이어다.

3. 테이블 엔트리는 개별 heap 객체로 유지한다.  
   그래야 executor가 잡고 있는 포인터가 다른 스레드의 registry 확장 때문에 무효화되지 않는다.

4. 다른 테이블끼리는 병렬, 같은 테이블은 우선 직렬화한다.  
   이것이 이번 단계의 최소 정답이다.

5. registry와 table lock을 분리한다.  
   registry lock은 "엔트리 찾기/생성"만 보호하고, 실제 row 접근은 per-table lock이 보호해야 한다.

## 6. 선택할 자료구조

### 6.1 `TableRuntime` 는 그대로 둔다

현재 `TableRuntime` 필드는 대부분 그대로 재사용한다.

- `table_name`
- `columns`
- `rows`
- `row_count`
- `capacity`
- `next_id`
- `id_index_root`
- `loaded`

이 구조는 "한 테이블의 메모리 상태"라는 관점에서는 충분하다.

### 6.2 새로 도입할 registry entry

이번 단계에서 새로 필요한 것은 테이블 하나를 감싸는 상위 엔트리다.

```c
typedef struct TableRuntimeEntry {
    TableRuntime table;
    pthread_mutex_t lock;
    struct TableRuntimeEntry *next;
} TableRuntimeEntry;
```

의도는 명확하다.

- `table` : 실제 런타임 데이터
- `lock` : 같은 테이블에 대한 동시 접근 직렬화
- `next` : registry 연결

### 6.3 전역 registry

전역 싱글턴은 없애되, 전역 "레지스트리"는 필요하다.

```c
static TableRuntimeEntry *table_runtime_registry_head = NULL;
static pthread_mutex_t table_runtime_registry_lock = PTHREAD_MUTEX_INITIALIZER;
```

이 전역 상태는 "active table 하나"가 아니라 "모든 테이블 엔트리 목록"을 관리한다.

### 6.4 왜 동적 배열이 아니라 linked list인가

이번 단계에서는 linked list가 더 안전하다.

- 동적 배열은 `realloc()` 시 엔트리 주소가 바뀔 수 있다.
- executor가 `TableRuntime *` 또는 entry 포인터를 잡고 작업하는 동안 주소가 바뀌면 use-after-free 위험이 생긴다.
- linked list는 각 엔트리를 개별 `malloc()` 하므로 포인터 안정성이 있다.
- 테이블 수가 아주 크지 않은 현재 MVP 범위에서는 O(n) lookup 비용이 수용 가능하다.

성능 최적화보다 correctness가 우선이므로, 이 단계에서는 pointer-stable 구조가 더 중요하다.

## 7. 공개 API를 어떻게 바꿀 것인가

현재 공개 API의 핵심 문제는 `table_get_or_load()` 가 raw `TableRuntime *` 를 바로 반환한다는 점이다.  
이 방식은 멀티스레드 환경에서 호출자가 락 없이 내부 상태를 만지는 통로가 된다.

따라서 public API는 "획득/해제" 모델로 바꾼다.

### 7.1 새 handle 타입

`src/table_runtime.h` 에 아래 개념을 추가한다.

```c
struct TableRuntimeEntry;

typedef struct {
    struct TableRuntimeEntry *entry;
} TableRuntimeHandle;
```

### 7.2 새 public 함수

```c
int table_runtime_acquire(const char *table_name, TableRuntimeHandle *out_handle);
TableRuntime *table_runtime_handle_table(TableRuntimeHandle *handle);
void table_runtime_release(TableRuntimeHandle *handle);
void table_runtime_cleanup(void);
```

역할은 다음과 같다.

- `table_runtime_acquire()`  
  주어진 테이블 이름의 runtime entry를 찾거나 새로 만들고, 그 테이블 lock을 획득한 상태로 handle을 반환한다.

- `table_runtime_handle_table()`  
  handle이 잡고 있는 실제 `TableRuntime *` 에 접근한다.

- `table_runtime_release()`  
  해당 테이블 lock을 해제한다.

- `table_runtime_cleanup()`  
  프로그램 종료 시 registry 전체를 해제한다.

### 7.3 `table_get_or_load()` 의 처리 방안

이 함수는 public API에서 제거하거나 최소한 header에서 숨기고 `table_runtime.c` 내부 helper로 내린다.

이유는 단순하다.

- 이름은 "load"처럼 보이지만 실제로는 active runtime 교체를 수행해 의미가 잘못돼 있다.
- 새 구조에서는 raw pointer를 락 없이 꺼내는 API 자체가 위험하다.
- 구현 후에도 테스트나 executor가 계속 이 함수를 쓰면 리팩토링 효과가 반감된다.

## 8. 락 모델

### 8.1 registry lock

`table_runtime_registry_lock` 의 책임은 딱 하나다.

- 엔트리 lookup
- 엔트리 생성
- registry list 삽입
- cleanup 시 list 순회 시작 전 보호

이 lock은 짧게 잡고 빨리 놓는다.  
row 접근이나 B+Tree 접근까지 registry lock으로 보호하면 또 다른 전역 병목이 생긴다.

### 8.2 per-table lock

각 `TableRuntimeEntry.lock` 은 같은 테이블 접근만 막는다.

- `users` lock은 `users` 접근만 직렬화
- `orders` lock은 `orders` 접근만 직렬화
- 따라서 `users`와 `orders`는 동시에 처리 가능

이번 단계에서는 `pthread_mutex_t` 로 충분하다.

- 목표는 "다른 테이블 병렬화"
- 목표가 아니다: "같은 테이블 read/read 병렬화"
- mutex가 가장 단순하고 구현 실패 위험이 낮다

### 8.3 lock 획득 순서

데드락을 피하기 위해 순서를 고정한다.

1. registry lock 획득
2. 엔트리 검색 또는 생성
3. registry lock 해제
4. 해당 table lock 획득

중요한 규칙:

- 일반 요청 경로에서는 registry lock과 table lock을 오래 동시에 잡지 않는다.
- 한 요청이 두 개 이상의 table lock을 동시에 잡지 않는다.
- cleanup은 모든 worker가 종료된 뒤에만 호출한다.

### 8.4 왜 `pthread_rwlock_t` 를 지금 쓰지 않는가

`goal.md` 상 최종 시스템은 병렬 SQL 처리가 필요하지만, 지금 막아야 하는 핵심 결함은 "다른 테이블이 서로의 런타임을 날려버리는 문제"다.

`pthread_rwlock_t` 를 바로 넣으면 범위가 커진다.

- 같은 테이블 read/read 허용 규약까지 같이 설계해야 한다.
- `executor`가 언제 lock을 잡고 언제 풀지 더 세밀한 결정이 필요하다.
- 초기 구현의 버그 표면적이 커진다.

그래서 이번 단계는 per-table mutex까지만 간다.  
이 단계만으로도 "테이블끼리 병렬 처리 자체가 불가능한 문제"는 해결된다.

## 9. 구현 단계별 상세 계획

## 9.1 1단계: 빌드 시스템 준비

대상 파일:

- `Makefile`

작업:

- 컴파일과 링크에 `-pthread` 추가
- 테스트 바이너리 링크에도 동일 플래그 적용

이 단계가 필요한 이유:

- `pthread_mutex_t`
- `pthread_create`
- `pthread_join`

를 테스트와 엔진에서 사용할 예정이기 때문이다.

완료 기준:

- `make tests` 가 pthread 심볼 에러 없이 빌드된다.

## 9.2 2단계: table runtime header 재설계

대상 파일:

- `src/table_runtime.h`

작업:

- `struct TableRuntimeEntry;` 전방 선언 추가
- `TableRuntimeHandle` 추가
- 새 acquire/release API 선언 추가
- `table_get_or_load()` 선언 제거 또는 내부용으로 숨김
- 기존 저수준 함수는 유지
  - `table_init`
  - `table_free`
  - `table_reserve_if_needed`
  - `table_insert_row`
  - `table_get_row_by_slot`
  - `table_linear_scan_by_field`

의도:

- low-level table 조작 함수와 registry access 계층을 분리한다.
- benchmark처럼 스택 로컬 `TableRuntime` 를 직접 쓰는 코드는 계속 살린다.

완료 기준:

- 새 API만으로 executor와 테스트를 연결할 수 있다.

## 9.3 3단계: registry 구현 추가

대상 파일:

- `src/table_runtime.c`

추가할 내부 helper:

- `static TableRuntimeEntry *table_runtime_find_entry_locked(const char *table_name);`
- `static TableRuntimeEntry *table_runtime_create_entry(const char *table_name);`
- `static void table_runtime_destroy_entry(TableRuntimeEntry *entry);`

세부 작업:

1. 기존 전역 변수 삭제
   - `table_runtime_active`
   - `table_runtime_has_active`

2. registry 전역 변수 추가
   - `table_runtime_registry_head`
   - `table_runtime_registry_lock`

3. 엔트리 생성 로직 구현
   - `malloc(sizeof(TableRuntimeEntry))`
   - `memset(entry, 0, sizeof(*entry))`
   - `pthread_mutex_init(&entry->lock, NULL)`
   - `table_init(&entry->table)`
   - `table.table_name` 에 table_name 복사
   - list head에 삽입

4. acquire 구현
   - table_name 검증
   - registry lock 획득
   - 기존 엔트리 lookup
   - 없으면 새 엔트리 생성 후 삽입
   - registry lock 해제
   - entry lock 획득
   - handle 반환

5. release 구현
   - handle 검증
   - `pthread_mutex_unlock()`
   - handle 초기화

6. cleanup 구현 변경
   - registry 전체 순회
   - 각 entry에 대해 `table_free(&entry->table)`
   - `pthread_mutex_destroy(&entry->lock)`
   - entry free
   - head NULL로 리셋

완료 기준:

- 다른 테이블을 acquire해도 기존 테이블 데이터가 삭제되지 않는다.
- registry에 같은 이름의 엔트리가 중복 생성되지 않는다.

## 9.4 4단계: executor를 새 runtime access 규약으로 교체

대상 파일:

- `src/executor.c`

변경 포인트 1: INSERT 경로

- 기존:
  - `table = table_get_or_load(stmt->table_name);`
- 변경:
  - `TableRuntimeHandle handle;`
  - `table_runtime_acquire(stmt->table_name, &handle);`
  - `table = table_runtime_handle_table(&handle);`
  - `table_insert_row(table, stmt, &row_index);`
  - `table_runtime_release(&handle);`

변경 포인트 2: SELECT 경로

- SELECT는 runtime row를 읽는 동안 테이블 상태가 바뀌면 안 된다.
- 따라서 `executor_prepare_projection()`, `executor_collect_all_rows()`, `executor_collect_rows_by_id()`, `executor_collect_rows_by_scan()` 에 필요한 table 접근이 끝날 때까지 lock을 유지한다.
- 결과 row 복사가 모두 끝난 뒤 release 한다.

중요한 구현 규칙:

- 조기 `return FAILURE;` 경로마다 release 누락이 없어야 한다.
- 이를 위해 `goto cleanup;` 패턴으로 정리하는 것이 안전하다.

완료 기준:

- executor가 더 이상 active runtime 개념에 의존하지 않는다.
- 서로 다른 테이블 요청이 서로의 데이터를 지우지 않는다.

## 9.5 5단계: 기존 단위 테스트 계약 뒤집기

대상 파일:

- `tests/test_table_runtime.c`
- `tests/test_executor.c`

### `tests/test_table_runtime.c` 변경 내용

기존의 잘못된 기대:

- `other_users` 로 전환하면 기존 row가 사라진다.

새 기대:

1. `runtime_users` 에 Alice, Bob 삽입
2. `other_users` 를 acquire하고 Carol 삽입
3. 다시 `runtime_users` 를 acquire
4. 기존 row 2개와 `next_id == 3` 유지 확인
5. `other_users` 도 독립적으로 `row_count == 1`, `next_id == 2` 확인

필수 검증 항목:

- 서로 다른 테이블의 `next_id` 가 독립적으로 증가한다.
- 서로 다른 테이블의 `id_index_root` 가 독립적으로 존재한다.
- 같은 이름을 다시 acquire하면 같은 logical table 상태를 본다.

### `tests/test_executor.c` 변경 내용

새 시나리오 추가:

1. `executor_users` 에 2행 삽입
2. `executor_orders` 에 1행 삽입
3. 다시 `executor_users` 조회
4. `executor_orders` 가 끼어들었어도 `executor_users` 데이터가 그대로 남아 있는지 확인

완료 기준:

- "테이블 전환 시 초기화" 관련 검증이 완전히 제거된다.
- 새 테스트가 "테이블 간 상태 보존"을 회귀 기준으로 삼는다.

## 9.6 6단계: 병렬 처리 회귀 테스트 추가

대상 파일:

- 새 파일 `tests/test_table_runtime_concurrency.c`
- `Makefile`
- `tests/run_tests.sh`

이 테스트는 이번 계획의 핵심 증거가 된다.  
단순히 기능 테스트만 바꾸면 "다른 테이블이 동시에 접근돼도 안전한지"가 증명되지 않는다.

### 테스트 시나리오

스레드 2개를 만든다.

- Thread A: `table_a` 에 1000번 insert
- Thread B: `table_b` 에 1000번 insert

각 스레드는 반복마다:

1. `table_runtime_acquire(table_name, &handle)`
2. `table_insert_row(table, &stmt, &row_index)`
3. `table_runtime_release(&handle)`

모든 스레드 종료 후 검증:

- `table_a.row_count == 1000`
- `table_b.row_count == 1000`
- `table_a.next_id == 1001`
- `table_b.next_id == 1001`
- 어느 테이블도 다른 테이블 삽입 때문에 초기화되지 않음

이 테스트가 증명하는 것:

- 다른 테이블끼리 상태가 섞이지 않는다.
- registry가 멀티스레드 환경에서 중복 생성/데이터 삭제를 일으키지 않는다.
- per-table lock 구조가 최소한의 병렬 독립성을 제공한다.

### 주의사항

- 같은 테이블에 대한 병렬 insert 테스트는 이번 단계의 핵심 검증이 아니다.
- 이번 단계는 "cross-table isolation" 증명이 우선이다.

## 9.7 7단계: 종료/정리 경로 검증

대상 파일:

- `src/main.c`
- 테스트 코드 일부

작업:

- `table_runtime_cleanup()` 가 이제 active table 하나가 아니라 registry 전체를 정리한다는 점을 반영
- 테스트 시작 전 cleanup, 테스트 종료 후 cleanup 호출은 유지
- cleanup은 worker join 이후 호출해야 한다는 사용 규약을 주석 또는 문서로 명시

완료 기준:

- 여러 테이블을 만든 뒤 cleanup해도 메모리 누수나 double free가 없다.

## 10. 함수 단위 변경 체크리스트

구현 중 빠뜨리기 쉬운 항목을 함수 기준으로 정리한다.

### `src/table_runtime.c`

- `table_init()`  
  그대로 유지. registry entry 생성 시 내부 table 초기화에 재사용.

- `table_free()`  
  그대로 유지. cleanup 시 entry별 해제에 사용.

- `table_reserve_if_needed()`  
  그대로 유지. lock은 상위 acquire 계층이 보장.

- `table_insert_row()`  
  내부 로직은 대부분 유지. 호출자는 반드시 table lock을 잡고 있어야 한다는 전제를 문서화.

- `table_linear_scan_by_field()`  
  내부 로직 유지. 호출자는 반드시 table lock을 잡고 있어야 한다.

- `table_get_row_by_slot()`  
  내부 로직 유지. 호출자는 반드시 table lock을 잡고 있어야 한다.

### `src/executor.c`

- `executor_execute_insert()`  
  acquire/release 누락 금지

- `executor_execute_select()`  
  result row 복사가 끝나기 전에 release하지 않도록 주의

### `tests/*`

- 새 API에 맞춰 handle acquire/release 사용
- 테스트 중 raw `TableRuntime *` 를 얻더라도 lock이 잡혀 있는 범위 안에서만 검사

## 11. 리스크와 대응

### 리스크 1: release 누락으로 인한 데드락

원인:

- `executor_execute_select()` 는 실패 경로가 많다.

대응:

- handle을 함수 초반에 선언
- `status` 변수 사용
- 하나의 `cleanup:` 라벨에서 release

### 리스크 2: registry 중복 생성

원인:

- 두 스레드가 동시에 같은 새 테이블을 처음 접근할 수 있다.

대응:

- lookup과 create를 반드시 registry lock 안에서 수행

### 리스크 3: 포인터 무효화

원인:

- registry를 배열로 구현하면 확장 시 주소가 바뀔 수 있다.

대응:

- entry 개별 heap 할당 + linked list 사용

### 리스크 4: 빌드 실패

원인:

- `pthread` 심볼 링크 누락

대응:

- `Makefile` 에 `-pthread` 명시

### 리스크 5: cleanup과 worker 동시 실행

원인:

- 아직 서버가 없지만, 나중에 shutdown 순서를 잘못 잡으면 use-after-free 가능

대응:

- 이 함수의 사용 규약을 문서화: cleanup은 모든 worker 종료 후 호출

## 12. 이번 단계의 완료 조건

아래 조건을 모두 만족하면 이번 계획 범위는 완료다.

- `table_runtime_active` 싱글턴이 사라진다.
- 다른 테이블 이름 접근이 기존 테이블 데이터를 삭제하지 않는다.
- executor가 새 registry access API를 사용한다.
- `test_table_runtime` 가 "전환 시 초기화" 대신 "테이블별 상태 유지"를 검증한다.
- 새로운 concurrency test가 통과한다.
- 서로 다른 테이블 요청은 동시에 실행 가능한 구조가 된다.
- 같은 테이블은 여전히 mutex로 직렬화돼 correctness를 우선 보장한다.

## 13. 이번 단계가 끝난 뒤 얻는 것

이 리팩토링이 끝나면 비로소 다음 논의가 의미를 가진다.

- per-table `pthread_rwlock_t` 로 같은 테이블 read/read 병렬화
- executor 결과 반환형 리팩토링
- thread pool을 붙인 API 서버 구현

반대로 이 단계를 건너뛰고 바로 thread pool이나 table-level lock 문서만 추가하면, 실제 엔진은 여전히 다른 테이블끼리 공존하지 못하기 때문에 `goal.md` 의 병렬 처리 요구사항을 만족할 수 없다.

## 14. 권장 구현 순서 한 줄 요약

`Makefile(-pthread) -> table_runtime registry/handle API -> executor 마이그레이션 -> 기존 테스트 계약 수정 -> cross-table concurrency test 추가 -> cleanup 검증`
