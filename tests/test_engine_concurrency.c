#include "engine.h"
#include "table_runtime.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define REPEAT_COUNT 100

typedef struct {
    const char *sql;
    int repeat_count;
    int expect_success;
} QueryThreadArgs;

static int assert_true(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "[FAIL] %s\n", message);
        return FAILURE;
    }
    return SUCCESS;
}

static int run_sql_once(const char *sql, int expect_success) {
    QueryResult result;
    int status;
    int ok;

    query_result_init(&result);
    status = engine_execute_sql(sql, &result);
    ok = expect_success ? (status == SUCCESS && result.success)
                        : (status == FAILURE && !result.success);
    if (!ok) {
        fprintf(stderr, "[FAIL] SQL expectation failed: %s\n", sql);
        if (result.error[0] != '\0') {
            fprintf(stderr, "       error: %s\n", result.error);
        }
    }
    query_result_free(&result);
    return ok ? SUCCESS : FAILURE;
}

static void *run_query_worker(void *arg) {
    QueryThreadArgs *args;
    int i;

    args = (QueryThreadArgs *)arg;
    for (i = 0; i < args->repeat_count; i++) {
        if (run_sql_once(args->sql, args->expect_success) != SUCCESS) {
            return (void *)1;
        }
    }

    return NULL;
}

static int run_two_workers(QueryThreadArgs *left_args,
                           QueryThreadArgs *right_args,
                           const char *message) {
    pthread_t left_thread;
    pthread_t right_thread;
    void *thread_result;

    if (assert_true(pthread_create(&left_thread, NULL, run_query_worker,
                                   left_args) == 0, message) != SUCCESS) {
        return FAILURE;
    }

    if (assert_true(pthread_create(&right_thread, NULL, run_query_worker,
                                   right_args) == 0, message) != SUCCESS) {
        pthread_join(left_thread, NULL);
        return FAILURE;
    }

    if (assert_true(pthread_join(left_thread, &thread_result) == 0,
                    "left worker should join") != SUCCESS ||
        assert_true(thread_result == NULL,
                    "left worker should finish successfully") != SUCCESS ||
        assert_true(pthread_join(right_thread, &thread_result) == 0,
                    "right worker should join") != SUCCESS ||
        assert_true(thread_result == NULL,
                    "right worker should finish successfully") != SUCCESS) {
        return FAILURE;
    }

    return SUCCESS;
}

static int assert_table_row_count(const char *table_name, int expected_count) {
    TableRuntimeHandle handle;
    TableRuntime *table;
    int status;

    handle.entry = NULL;
    if (assert_true(table_runtime_acquire_read(table_name, &handle) == SUCCESS,
                    "table should be readable") != SUCCESS) {
        return FAILURE;
    }

    table = table_runtime_handle_table(&handle);
    status = assert_true(table != NULL && table->row_count == expected_count,
                         "table row count should match expected value");
    table_runtime_release(&handle);
    return status;
}

int main(void) {
    QueryThreadArgs select_a;
    QueryThreadArgs select_b;
    QueryThreadArgs insert_users;
    QueryThreadArgs insert_users_again;
    QueryThreadArgs insert_orders;
    int i;

    table_runtime_cleanup();

    for (i = 0; i < REPEAT_COUNT; i++) {
        if (run_sql_once("SELECT * FROM missing_engine_table;", 0) != SUCCESS) {
            return EXIT_FAILURE;
        }
    }
    if (assert_true(table_runtime_registry_count() == 0,
                    "missing table reads should not create registry entries") != SUCCESS) {
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }

    if (run_sql_once("INSERT INTO concurrent_users (name, age) VALUES ('Alice', 30);", 1) != SUCCESS ||
        run_sql_once("INSERT INTO concurrent_users (name, age) VALUES ('Bob', 25);", 1) != SUCCESS) {
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }

    select_a.sql = "SELECT * FROM concurrent_users WHERE age >= 0;";
    select_a.repeat_count = REPEAT_COUNT;
    select_a.expect_success = 1;
    select_b = select_a;

    if (run_two_workers(&select_a, &select_b,
                        "same-table select workers should start") != SUCCESS) {
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }

    insert_users.sql =
        "INSERT INTO concurrent_users (name, age) VALUES ('Worker', 44);";
    insert_users.repeat_count = REPEAT_COUNT;
    insert_users.expect_success = 1;

    if (run_two_workers(&select_a, &insert_users,
                        "same-table select and insert workers should start") != SUCCESS) {
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }

    insert_users_again = insert_users;
    if (run_two_workers(&insert_users, &insert_users_again,
                        "same-table insert workers should start") != SUCCESS) {
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }

    insert_orders.sql =
        "INSERT INTO concurrent_orders (name, age) VALUES ('Order', 10);";
    insert_orders.repeat_count = REPEAT_COUNT;
    insert_orders.expect_success = 1;

    if (run_two_workers(&select_a, &insert_orders,
                        "cross-table select and insert workers should start") != SUCCESS) {
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }

    if (assert_table_row_count("concurrent_users", 2 + REPEAT_COUNT * 3) != SUCCESS ||
        assert_table_row_count("concurrent_orders", REPEAT_COUNT) != SUCCESS) {
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }

    table_runtime_cleanup();
    puts("[PASS] engine_concurrency");
    return EXIT_SUCCESS;
}
