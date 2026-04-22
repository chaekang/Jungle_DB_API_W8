#include "engine.h"
#include "query_result.h"
#include "table_runtime.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENGINE_THREAD_COUNT 4
#define ENGINE_INSERTS_PER_THREAD 150

typedef struct {
    int worker_id;
    volatile int failed;
} EngineWorkerArgs;

static int assert_true(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "[FAIL] %s\n", message);
        return FAILURE;
    }
    return SUCCESS;
}

static void *engine_insert_worker(void *arg) {
    EngineWorkerArgs *worker_args;
    int i;

    worker_args = (EngineWorkerArgs *)arg;
    for (i = 0; i < ENGINE_INSERTS_PER_THREAD; i++) {
        QueryResult result;
        char sql[MAX_SQL_LENGTH];

        query_result_init(&result);
        snprintf(sql, sizeof(sql),
                 "INSERT INTO engine_concurrency_users (name, age) VALUES ('worker_%d_%d', %d);",
                 worker_args->worker_id, i, 20 + worker_args->worker_id);
        if (engine_execute_sql(sql, &result) != SUCCESS) {
            worker_args->failed = 1;
            query_result_free(&result);
            return NULL;
        }
        query_result_free(&result);
    }

    return NULL;
}

int main(void) {
    pthread_t workers[ENGINE_THREAD_COUNT];
    EngineWorkerArgs worker_args[ENGINE_THREAD_COUNT];
    QueryResult result;
    int i;
    int expected_rows;

    table_runtime_cleanup();

    for (i = 0; i < ENGINE_THREAD_COUNT; i++) {
        memset(&worker_args[i], 0, sizeof(worker_args[i]));
        worker_args[i].worker_id = i + 1;
        if (assert_true(pthread_create(&workers[i], NULL, engine_insert_worker,
                                       &worker_args[i]) == 0,
                        "engine insert worker should start") != SUCCESS) {
            return EXIT_FAILURE;
        }
    }

    for (i = 0; i < ENGINE_THREAD_COUNT; i++) {
        if (assert_true(pthread_join(workers[i], NULL) == 0,
                        "engine insert worker should join") != SUCCESS ||
            assert_true(worker_args[i].failed == 0,
                        "engine insert worker should finish without SQL errors") != SUCCESS) {
            return EXIT_FAILURE;
        }
    }

    expected_rows = ENGINE_THREAD_COUNT * ENGINE_INSERTS_PER_THREAD;

    query_result_init(&result);
    if (assert_true(engine_execute_sql("SELECT * FROM engine_concurrency_users;", &result) ==
                        SUCCESS,
                    "engine should select all rows after concurrent inserts") != SUCCESS ||
        assert_true(result.kind == QUERY_RESULT_TABLE,
                    "concurrent select should return table data") != SUCCESS ||
        assert_true(result.row_count == expected_rows,
                    "concurrent inserts should preserve every row") != SUCCESS ||
        assert_true(strcmp(result.rows[0][0], "1") == 0,
                    "first generated id should remain 1") != SUCCESS ||
        assert_true(strcmp(result.rows[result.row_count - 1][0], "600") == 0,
                    "last generated id should advance sequentially") != SUCCESS) {
        query_result_free(&result);
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }
    query_result_free(&result);

    query_result_init(&result);
    if (assert_true(engine_execute_sql(
                        "SELECT name FROM engine_concurrency_users WHERE id = 600;",
                        &result) == SUCCESS,
                    "engine should support indexed lookup after concurrent inserts") != SUCCESS ||
        assert_true(result.index_used == 1,
                    "concurrent table id lookup should still use the index") != SUCCESS ||
        assert_true(result.row_count == 1,
                    "concurrent table id lookup should find one row") != SUCCESS ||
        assert_true(strstr(result.rows[0][0], "worker_") == result.rows[0][0],
                    "concurrent table row payload should remain intact") != SUCCESS) {
        query_result_free(&result);
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }
    query_result_free(&result);

    table_runtime_cleanup();
    puts("[PASS] engine_concurrency");
    return EXIT_SUCCESS;
}
