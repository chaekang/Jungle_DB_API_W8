# Edge Case Fix Report

작성일: 2026-04-22

이 문서는 다음 4개 문제를 실제 코드로 수정한 내용과 검증 과정을 정리한 기록이다.

- 1번: `long long` 범위를 넘는 큰 숫자 비교가 조용히 잘못될 수 있는 문제
- 3번: 파일 모드에서 SQL 실행 실패가 exit code에 반영되지 않는 문제
- 4번: 긴 SQL / 긴 식별자 / 긴 문자열 리터럴에서 실패 이유가 흐릿하게 보이는 문제
- 7번: 문자열 컬럼의 숫자처럼 보이는 값이 숫자 비교로 처리될 수 있는 문제

## 1. 큰 숫자 overflow 비교 문제

### 어떤 문제가 있었는가

기존 구현은 숫자처럼 보이는 문자열을 `strtoll()`로 바로 변환해서 비교했다.  
이때 값이 `long long` 범위를 넘으면 overflow가 나도 호출부에서 그 실패를 명시적으로 검증하지 않았다.

예를 들어 아래 같은 조건은 잘못된 입력이어야 한다.

```sql
SELECT name FROM users WHERE age > 999999999999999999999999;
```

### 왜 문제인가

이 문제는 단순 에러가 아니라 "조용한 오답"으로 이어질 수 있다는 점이 가장 위험하다.  
사용자는 잘못된 숫자를 넣었는데도 에러 대신 엉뚱한 비교 결과를 받게 되고, 그러면 버그를 발견하기 더 어려워진다.

### 어떻게 고쳤는가

다음 순서로 수정했다.

1. `utils_try_parse_integer()`를 추가해 정수 형식과 overflow를 함께 검증하도록 바꿨다.
2. `utils_compare_values()`는 내부적으로 검증된 정수 비교 함수를 사용하도록 바꿨다.
3. `table_runtime`의 선형 스캔 비교에서 숫자 컬럼은 검증된 정수 비교만 허용하도록 바꿨다.
4. overflow가 발생하면 `table_runtime_get_last_error()`를 통해 `"Invalid integer literal for numeric column."` 오류를 실행 결과까지 전달하도록 연결했다.

### 해결 과정에서 생긴 문제와 해결

처음에는 `executor` 쪽에서만 overflow를 막을까도 생각했지만, 그렇게 하면 인덱스 경로만 안전해지고 일반 선형 스캔 경로는 그대로 남게 된다.  
그래서 최종적으로는 비교가 실제로 일어나는 `utils` + `table_runtime` 레벨에서 같이 막는 방식으로 바꿨다.

### 결과적으로 어떻게 동작하게 되었는가

이제 숫자 컬럼에 대해 범위를 넘는 정수를 조건값으로 넣으면 성공처럼 처리되지 않고 명확한 실패로 반환된다.

검증 테스트:

- `tests/test_engine.c`
- `tests/test_table_runtime.c`

## 3. 파일 모드 실패 미전파 문제

### 어떤 문제가 있었는가

기존 `main_run_file_mode()`는 파일 안의 SQL 문을 순서대로 실행했지만, 중간에 실패한 문장이 있어도 전체 종료 상태를 계속 `SUCCESS`로 반환했다.

예를 들어 아래 파일을 실행해도:

```sql
SELECT * FROM missing_table;
```

실패 출력은 보이지만 프로세스 exit code는 0일 수 있었다.

### 왜 문제인가

이 문제는 자동화 환경에서 특히 치명적이다.  
CI, 스크립트, 배치 작업은 보통 exit code로 성공/실패를 판단하는데, 실제 실패가 0으로 끝나면 상위 자동화가 잘못된 성공으로 오해하게 된다.

### 어떻게 고쳤는가

`main_run_file_mode()`에 `overall_status`를 추가해서:

1. 각 SQL 문 실행 결과를 누적하고
2. 중간 실패가 하나라도 있으면 최종적으로 `FAILURE`를 반환하며
3. 세미콜론 누락도 실패 상태에 반영하도록 바꿨다.

### 해결 과정에서 생긴 문제와 해결

실패를 바로 만나면 즉시 종료할지, 끝까지 실행할지 선택지가 있었다.  
최종적으로는 기존 동작 흐름을 최대한 보존하기 위해 "계속 실행하되, 최종 반환값만 실패로 누적"하는 쪽으로 정리했다.

### 결과적으로 어떻게 동작하게 되었는가

이제 파일 모드에서 문장 하나라도 실패하면 프로세스가 non-zero exit code로 끝난다.

검증 테스트:

- `tests/test_cases/file_mode_failure.sql`
- `tests/run_tests.sh`의 `run_sql_expect_failure_exit()`

## 4. 긴 입력에서 에러 이유가 흐릿한 문제

### 어떤 문제가 있었는가

기존 엔진은 tokenizer나 parser에서 실패해도 대체로 아래처럼 뭉뚱그린 메시지만 반환했다.

- `Failed to tokenize SQL.`
- `Failed to parse SQL.`

그래서 실제 원인이:

- SQL 전체 길이 초과인지
- 식별자가 너무 긴지
- 문자열 리터럴이 너무 긴지

같은 중요한 정보가 사용자에게 잘 전달되지 않았다.

### 왜 문제인가

입력 제한 문제는 "왜 안 되는지"를 알아야 바로 고칠 수 있다.  
원인이 불명확하면 사용자는 SQL 문법을 의심하게 되고, 실제로는 길이 제한 문제인데도 디버깅 시간이 불필요하게 늘어난다.

### 어떻게 고쳤는가

다음과 같이 에러 전파 구조를 바꿨다.

1. `tokenizer`에 thread-local 마지막 에러 버퍼를 추가했다.
2. `parser`에도 thread-local 마지막 에러 버퍼를 추가했다.
3. `engine_execute_sql()`가 generic 메시지 대신 tokenizer/parser의 구체적인 마지막 에러를 그대로 넘기도록 바꿨다.
4. `MAX_SQL_LENGTH`를 넘는 경우는 엔진 초입에서 `"SQL is too long."`으로 바로 실패시키도록 했다.
5. 서버 JSON 파서도 SQL 문자열이 너무 긴 경우 `413 Payload Too Large`와 `"SQL string is too long."`을 돌려주도록 분기했다.

### 해결 과정에서 생긴 문제와 해결

이 프로젝트는 멀티스레드 서버 경로가 있어서 전역 에러 문자열 하나를 공유하면 스레드 간 충돌이 날 수 있었다.  
그래서 일반 전역 버퍼 대신 thread-local 버퍼를 써서 각 요청/스레드가 자기 에러 상태를 따로 갖도록 정리했다.

### 결과적으로 어떻게 동작하게 되었는가

이제 긴 입력이 들어오면 원인에 맞는 에러가 보인다.

예시:

- 너무 긴 SQL 전체: `SQL is too long.`
- 너무 긴 식별자: `Identifier is too long.`
- 너무 긴 문자열 리터럴: `String literal is too long.`

검증 테스트:

- `tests/test_engine.c`

## 7. 숫자처럼 보이는 문자열 비교 문제

### 어떤 문제가 있었는가

기존 비교 로직은 양쪽 값이 숫자 모양이면 무조건 숫자로 비교했다.  
이 방식은 문자열 컬럼에도 그대로 적용되어, 예를 들어 `'100'`과 `'9'` 같은 문자열이 문자열 비교가 아니라 숫자 비교로 처리될 수 있었다.

### 왜 문제인가

문자열 컬럼의 값은 앞의 0, 코드값, 문자열 정렬 순서 자체가 의미일 수 있다.  
그런데 숫자로 강제 비교하면 데이터 의미가 바뀌고, 특히 코드/식별값 비교 결과가 직관과 달라질 수 있다.

### 어떻게 고쳤는가

이 문제를 해결하려고 값 타입 정보를 구조체와 런타임 스키마에 추가했다.

1. `parser`가 literal의 종류를 `VALUE_KIND_INT` / `VALUE_KIND_STRING`으로 기록하게 했다.
2. `InsertStatement`와 `WhereClause`에 값 타입 정보를 추가했다.
3. `table_runtime`는 첫 INSERT 시 컬럼별 값 타입을 스키마처럼 저장하게 했다.
4. 이후 비교 시 숫자 컬럼만 숫자로 비교하고, 문자열 컬럼은 숫자처럼 보여도 문자열 비교를 하도록 바꿨다.
5. 스키마가 이미 정해진 테이블에는 이후 INSERT 값 타입도 맞는지 검증하게 해서, 비교 규칙이 중간에 흔들리지 않도록 했다.

### 해결 과정에서 생긴 문제와 해결

이 문제는 비교 함수만 고쳐서는 끝나지 않았다.  
왜냐하면 런타임은 현재 "값의 실제 SQL literal 타입"을 기억하지 않고 그냥 문자열만 저장하고 있었기 때문이다.

그래서 단순 비교 로직 변경이 아니라:

- parser 구조체 확장
- runtime 스키마 확장
- 수동으로 `InsertStatement`를 만드는 테스트/벤치 코드 보정

까지 같이 수정해야 했다.

### 결과적으로 어떻게 동작하게 되었는가

이제 문자열 컬럼에 저장된 `'100'` 과 `'9'`는 문자열 비교로 처리된다.  
즉 `WHERE code > '9'` 는 숫자 비교처럼 `'100'`을 잡아내지 않고, 문자열 정렬 규칙에 따라 처리된다.

검증 테스트:

- `tests/test_table_runtime.c`
- `tests/test_engine.c`

## 검증 과정

### 빌드/테스트 실행 중 만난 문제

처음에는 기존 WSL 경로로 `make tests`를 실행하려고 했지만, 현재 환경에서 WSL 디스크가 read-only fallback 상태로 올라오면서 `bash` 실행이 실패했다.

실패 예:

- WSL 배포 디스크 mount 오류
- `/bin/sh: bash: not found`

그래서 빌드 경로를 다시 찾아보니 로컬에 `C:\msys64` 툴체인이 설치되어 있었고, 그 경로의 `bash.exe`와 `gcc`/`make`를 사용해 테스트를 진행했다.

### 실제 실행한 검증

실행 명령:

```powershell
& 'C:\msys64\usr\bin\bash.exe' -lc 'cd /c/Jungle/Jungle_DB_API_W8 && make tests'
```

최종 결과:

```text
Results: 18 passed, 0 failed
```

포함된 검증 범위:

- 기존 단위 테스트
- 기존 SQL 파일 기반 테스트
- 새로 추가한 `test_engine`
- 새로 추가한 파일 모드 exit code 검증

## 수정 파일 목록

- `src/engine.c`
- `src/executor.c`
- `src/main.c`
- `src/parser.c`
- `src/parser.h`
- `src/server.c`
- `src/table_runtime.c`
- `src/table_runtime.h`
- `src/tokenizer.c`
- `src/tokenizer.h`
- `src/utils.c`
- `src/utils.h`
- `src/benchmark.c`
- `tests/test_engine.c`
- `tests/test_executor.c`
- `tests/test_table_runtime.c`
- `tests/test_table_runtime_concurrency.c`
- `tests/run_tests.sh`
- `tests/test_cases/file_mode_failure.sql`

## 요약

이번 수정으로 4개 문제는 다음 상태가 되었다.

- 1번: overflow 입력이 조용한 오답이 아니라 명확한 실패로 바뀌었다.
- 3번: 파일 모드 실패가 실제 exit code로 전파된다.
- 4번: 긴 입력 실패 원인이 구체적으로 드러난다.
- 7번: 문자열 컬럼의 숫자처럼 보이는 값은 문자열 의미를 유지한 채 비교된다.
