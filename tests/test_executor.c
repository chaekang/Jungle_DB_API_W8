#include "bptree.h"
#include "engine.h"
#include "query_result.h"
#include "table_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int assert_true(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "[FAIL] %s\n", message);
        return FAILURE;
    }
    return SUCCESS;
}

static int execute_sql_expect_success(const char *sql, QueryResult *result) {
    query_result_init(result);
    if (engine_execute_sql(sql, result) != SUCCESS) {
        return FAILURE;
    }
    return SUCCESS;
}

int main(void) {
    QueryResult result;
    TableRuntimeHandle users_handle;
    TableRuntimeHandle orders_handle;
    TableRuntime *table;
    char **row;
    int row_index;

    table_runtime_cleanup();
    users_handle.entry = NULL;
    users_handle.lock_mode = TABLE_RUNTIME_LOCK_NONE;
    orders_handle.entry = NULL;
    orders_handle.lock_mode = TABLE_RUNTIME_LOCK_NONE;

    if (assert_true(execute_sql_expect_success(
                        "INSERT INTO executor_users (name, age) VALUES ('Alice', 30);",
                        &result) == SUCCESS,
                    "engine should insert first row into executor_users") != SUCCESS ||
        assert_true(strcmp(result.message, "1 row inserted into executor_users.") == 0,
                    "insert should return a success message") != SUCCESS) {
        query_result_free(&result);
        return EXIT_FAILURE;
    }
    query_result_free(&result);

    if (assert_true(execute_sql_expect_success(
                        "INSERT INTO executor_users (name, age) VALUES ('Bob', 25);",
                        &result) == SUCCESS,
                    "engine should insert second row into executor_users") != SUCCESS) {
        query_result_free(&result);
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }
    query_result_free(&result);

    if (assert_true(table_runtime_acquire_read("executor_users", &users_handle) == SUCCESS,
                    "should acquire executor_users state") != SUCCESS) {
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }

    table = table_runtime_handle_table(&users_handle);
    if (assert_true(table != NULL, "executor_users table should be available after insert") != SUCCESS ||
        assert_true(table->row_count == 2, "executor_users should contain two rows") != SUCCESS ||
        assert_true(table->id_index_root != NULL, "id index should be built on insert") != SUCCESS ||
        assert_true(bptree_search(table->id_index_root, 2, &row_index) == SUCCESS,
                    "id index should find the second row") != SUCCESS ||
        assert_true(row_index == 1, "id 2 should map to row_index 1") != SUCCESS) {
        table_runtime_release(&users_handle);
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }
    table_runtime_release(&users_handle);

    if (assert_true(execute_sql_expect_success(
                        "INSERT INTO executor_orders (name, age) VALUES ('Order-1', 10);",
                        &result) == SUCCESS,
                    "engine should insert into executor_orders independently") != SUCCESS) {
        query_result_free(&result);
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }
    query_result_free(&result);

    if (assert_true(table_runtime_acquire_read("executor_orders", &orders_handle) == SUCCESS,
                    "should acquire executor_orders state") != SUCCESS) {
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }

    table = table_runtime_handle_table(&orders_handle);
    row = table_get_row_by_slot(table, 0);
    if (assert_true(table != NULL, "executor_orders table should exist") != SUCCESS ||
        assert_true(table->row_count == 1, "executor_orders should contain one row") != SUCCESS ||
        assert_true(table->next_id == 2, "executor_orders next_id should advance independently") != SUCCESS ||
        assert_true(row != NULL, "executor_orders row lookup should succeed") != SUCCESS ||
        assert_true(strcmp(row[1], "Order-1") == 0, "executor_orders data should be isolated") != SUCCESS) {
        table_runtime_release(&orders_handle);
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }
    table_runtime_release(&orders_handle);

    if (assert_true(execute_sql_expect_success(
                        "SELECT name FROM executor_users WHERE id = 2;",
                        &result) == SUCCESS,
                    "engine should support id equality select") != SUCCESS ||
        assert_true(result.kind == QUERY_RESULT_TABLE, "SELECT should return table data") != SUCCESS ||
        assert_true(result.index_used == 1, "id equality select should report index usage") != SUCCESS ||
        assert_true(result.row_count == 1, "id equality select should return one row") != SUCCESS ||
        assert_true(strcmp(result.rows[0][0], "Bob") == 0,
                    "id equality select should return Bob") != SUCCESS) {
        query_result_free(&result);
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }
    query_result_free(&result);

    if (assert_true(execute_sql_expect_success(
                        "SELECT name FROM executor_users WHERE age >= 27;",
                        &result) == SUCCESS,
                    "engine should support non-id select") != SUCCESS ||
        assert_true(result.index_used == 0, "non-id select should use linear scan") != SUCCESS ||
        assert_true(result.row_count == 1, "non-id select should return Alice only") != SUCCESS ||
        assert_true(strcmp(result.rows[0][0], "Alice") == 0,
                    "non-id select should return Alice") != SUCCESS) {
        query_result_free(&result);
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }
    query_result_free(&result);

    if (assert_true(execute_sql_expect_success(
                        "SELECT * FROM executor_users WHERE age > 100;",
                        &result) == SUCCESS,
                    "engine should allow empty SELECT result sets") != SUCCESS ||
        assert_true(result.kind == QUERY_RESULT_TABLE,
                    "empty select should still return table shape") != SUCCESS ||
        assert_true(result.column_count == 3,
                    "SELECT * should include every runtime column") != SUCCESS ||
        assert_true(strcmp(result.columns[0], "id") == 0,
                    "SELECT * should expose id as the first column") != SUCCESS ||
        assert_true(result.row_count == 0,
                    "empty select should return zero rows") != SUCCESS) {
        query_result_free(&result);
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }
    query_result_free(&result);

    query_result_init(&result);
    if (assert_true(engine_execute_sql(
                        "INSERT INTO executor_users (id, name, age) VALUES (7, 'Charlie', 35);",
                        &result) == FAILURE,
                    "engine should reject explicit id inserts") != SUCCESS ||
        assert_true(strstr(result.error, "Explicit id values are not allowed") != NULL,
                    "explicit id rejection should use structured error") != SUCCESS) {
        query_result_free(&result);
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }
    query_result_free(&result);

    query_result_init(&result);
    if (assert_true(engine_execute_sql(
                        "INSERT INTO executor_users (name) VALUES ('OnlyName');",
                        &result) == FAILURE,
                    "engine should reject schema mismatch inserts") != SUCCESS ||
        assert_true(strstr(result.error, "schema") != NULL ||
                        strstr(result.error, "match") != NULL,
                    "schema mismatch should return a structured schema error") != SUCCESS) {
        query_result_free(&result);
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }
    query_result_free(&result);

    query_result_init(&result);
    if (assert_true(engine_execute_sql(
                        "SELECT nickname FROM executor_users;",
                        &result) == FAILURE,
                    "engine should reject unknown projection columns") != SUCCESS ||
        assert_true(strstr(result.error, "Column 'nickname' not found.") != NULL,
                    "unknown projection should report the missing column") != SUCCESS) {
        query_result_free(&result);
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }
    query_result_free(&result);

    query_result_init(&result);
    if (assert_true(engine_execute_sql("INSERT INTO broken VALUES (1);", &result) == FAILURE,
                    "engine should reject malformed INSERT syntax") != SUCCESS ||
        assert_true(result.error[0] != '\0',
                    "malformed SQL should populate a structured parse error") != SUCCESS) {
        query_result_free(&result);
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }
    query_result_free(&result);

    query_result_init(&result);
    if (assert_true(engine_execute_sql(
                        "DELETE FROM executor_users WHERE name = 'Bob';",
                        &result) == FAILURE,
                    "engine should reject delete in memory runtime mode") != SUCCESS ||
        assert_true(strstr(result.error, "DELETE is not supported") != NULL,
                    "unsupported delete should use structured error") != SUCCESS) {
        query_result_free(&result);
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }
    query_result_free(&result);

    query_result_init(&result);
    if (assert_true(engine_execute_sql("SELECT * FROM missing_executor_table;", &result) ==
                        FAILURE,
                    "missing table select should fail") != SUCCESS ||
        assert_true(strstr(result.error, "not found") != NULL,
                    "missing table select should report not found") != SUCCESS) {
        query_result_free(&result);
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }
    query_result_free(&result);

    if (assert_true(table_runtime_acquire_read("executor_users", &users_handle) == SUCCESS,
                    "executor_users should still be acquireable after cross-table access") != SUCCESS) {
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }

    table = table_runtime_handle_table(&users_handle);
    row = table_get_row_by_slot(table, 1);
    if (assert_true(row != NULL, "runtime row lookup should still succeed") != SUCCESS ||
        assert_true(strcmp(row[0], "2") == 0, "second row id should remain 2") != SUCCESS ||
        assert_true(strcmp(row[1], "Bob") == 0, "Bob should remain after delete failure") != SUCCESS ||
        assert_true(table->row_count == 2, "executor_users row count should stay stable") != SUCCESS) {
        table_runtime_release(&users_handle);
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }
    table_runtime_release(&users_handle);

    table_runtime_cleanup();
    puts("[PASS] executor");
    return EXIT_SUCCESS;
}
