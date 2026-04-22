# Test Failure Fix Report

## Summary

이번 수정은 실패하거나 멈추던 테스트를 실제 재현한 뒤, 원인을 코드 레벨에서 분리해서 고친 기록이다.

확인된 문제는 네 가지였다.

1. 기존 테이블에 `id` 컬럼을 명시한 `INSERT`가 의도한 에러 메시지 대신 일반 schema mismatch 경로로 빠질 수 있었다.
2. `DELETE` 미지원 경로가 `QueryResult`에는 에러를 기록하면서도 엔진 반환값은 성공처럼 보일 수 있었다.
3. 서버 종료 시 accept 스레드가 `accept()` 블로킹에서 빠져나오지 못해 `test_server`가 멈출 수 있었다.
4. 일부 동시성 테스트 시나리오에서 클라이언트가 먼저 연결을 끊으면 worker가 에러 응답을 쓰다가 `SIGPIPE`를 맞을 수 있었다.

## Root Cause And Fix

### 1. Explicit `id` insert validation was incomplete

파일: [src/table_runtime.c](/C:/Jungle/Jungle_DB_API_W8/src/table_runtime.c)

문제:
이미 schema가 잡힌 테이블에 대해 `INSERT INTO ... (id, ...) VALUES ...`를 실행하면, 코드가 먼저 컬럼 개수와 순서를 비교했다. 그래서 `id` 명시 금지라는 더 정확한 도메인 에러 대신 일반적인 `INSERT columns do not match table schema.`로 떨어질 수 있었다.

수정:
`table_validate_insert_schema()` 초반에 전달받은 컬럼 목록을 먼저 훑어서 `id`가 포함되어 있으면 즉시 `Explicit id values are not allowed.`를 반환하도록 바꿨다.

효과:
신규 테이블 생성 시점뿐 아니라 기존 테이블에 대한 재삽입 시도도 같은 정책으로 일관되게 막힌다.

### 2. Unsupported `DELETE` returned the wrong engine status

파일: [src/executor.c](/C:/Jungle/Jungle_DB_API_W8/src/executor.c)

문제:
`query_result_set_error()`는 "에러 상태를 결과 객체에 기록하는 작업" 자체가 성공하면 `SUCCESS`를 반환한다. 그런데 `executor_execute_delete()`가 그 반환값을 그대로 넘기고 있어서, `DELETE`가 미지원이어도 엔진 호출자는 SQL 실행이 성공한 것처럼 받아들일 수 있었다.

수정:
`executor_execute_delete()`와 unsupported default 분기에서:

- 먼저 `query_result_set_error(...)`로 구조화된 에러를 세팅하고
- 함수 반환값은 명시적으로 `FAILURE`를 돌려주도록 변경했다.

효과:
`QueryResult`의 내용과 `engine_execute_sql()`의 반환값이 서로 같은 의미를 가지게 됐다.

### 3. Server shutdown could hang in blocking `accept()`

파일: [src/server.c](/C:/Jungle/Jungle_DB_API_W8/src/server.c)

문제:
서버 정지 시 `listen_fd`를 다른 스레드에서 닫는 방식만으로는, Linux 환경에서 accept 스레드가 항상 즉시 깨어난다는 보장이 없었다. 실제로 `test_server`는 `server_join()`에서 accept 스레드를 기다리다가 멈췄다.

수정:
accept 루프를 직접 `accept()` 블로킹에 맡기지 않고 `poll(..., 100ms)` 기반으로 바꿨다.

- 100ms 단위로 listen socket readiness를 확인한다.
- `stop_requested`가 세팅되면 다음 poll tick에서 빠져나온다.
- `server_stop()`은 종료 플래그와 queue shutdown만 담당하도록 단순화했다.

효과:
서버 종료 경로가 OS별 소켓 close 타이밍에 덜 의존하게 됐고, 테스트에서 deterministic 하게 정리된다.

### 4. Worker could receive `SIGPIPE` while replying to a closed client

파일: [src/server.c](/C:/Jungle/Jungle_DB_API_W8/src/server.c)

문제:
병렬성/과부하 테스트는 일부러 연결만 열어두거나 중간에 닫는 시나리오를 만든다. 이때 worker가 malformed request에 대한 `400` 응답을 쓰는 순간 peer가 이미 닫혀 있으면 `send()`가 `SIGPIPE`를 발생시킬 수 있었다.

수정:
`server_send_all()`에서 `send()` 호출 시 `MSG_NOSIGNAL` 플래그를 사용하도록 변경했다.

효과:
peer disconnect는 이제 프로세스를 흔드는 signal이 아니라 일반적인 send failure로 처리된다.

## Verification

검증은 Docker 테스트 환경에서 현재 워크트리를 그대로 마운트해서 수행했다.

성공한 검증:

1. `build/tests/test_server`
2. `build/tests/test_executor`
3. `build/tests/test_tokenizer`
4. `build/tests/test_parser`
5. `build/tests/test_storage`
6. `build/tests/test_benchmark`
7. `build/tests/test_table_runtime`
8. `build/tests/test_table_runtime_concurrency`
9. `build/tests/test_bptree`
10. SQL file regression tests in [tests/run_tests.sh](/C:/Jungle/Jungle_DB_API_W8/tests/run_tests.sh)

최종 결과:

- `16 passed, 0 failed`

실행에 사용한 검증 명령:

```bash
docker run --rm -v "${PWD}:/app" -w /app jungle-db-api-w8-test bash -lc 'make clean >/dev/null && make sql_processor build/tests/test_tokenizer build/tests/test_parser build/tests/test_storage build/tests/test_benchmark build/tests/test_table_runtime build/tests/test_table_runtime_concurrency build/tests/test_bptree build/tests/test_executor build/tests/test_server >/dev/null && timeout 180s bash -x tests/run_tests.sh'
```

참고:
`Makefile`의 `tests` 타깃 레시피 자체는 `bash tests/run_tests.sh`이므로, 위 검증은 실제 테스트 레시피와 동일한 본체를 직접 실행한 것이다.
