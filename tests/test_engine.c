#include "engine.h"
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

int main(void) {
    QueryResult result;

    table_runtime_cleanup();

    query_result_init(&result);
    if (assert_true(engine_execute_sql(
                        "INSERT INTO engine_users (name, age) VALUES ('Alice', 30);",
                        &result) == SUCCESS,
                    "engine should execute INSERT successfully") != SUCCESS ||
        assert_true(result.success == 1, "insert result should be successful") != SUCCESS ||
        assert_true(strcmp(result.message, "1 row inserted into engine_users.") == 0,
                    "insert result should contain message") != SUCCESS) {
        query_result_free(&result);
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }
    query_result_free(&result);

    query_result_init(&result);
    if (assert_true(engine_execute_sql(
                        "SELECT name, age FROM engine_users WHERE id = 1;",
                        &result) == SUCCESS,
                    "engine should execute SELECT successfully") != SUCCESS ||
        assert_true(result.success == 1, "select result should be successful") != SUCCESS ||
        assert_true(result.kind == QUERY_RESULT_TABLE, "select should return table result") != SUCCESS ||
        assert_true(result.column_count == 2, "select should return two columns") != SUCCESS ||
        assert_true(result.row_count == 1, "select should return one row") != SUCCESS ||
        assert_true(strcmp(result.columns[0], "name") == 0, "first column should be name") != SUCCESS ||
        assert_true(strcmp(result.rows[0][0], "Alice") == 0, "selected row should contain Alice") != SUCCESS ||
        assert_true(strcmp(result.rows[0][1], "30") == 0, "selected row should contain age 30") != SUCCESS) {
        query_result_free(&result);
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }
    query_result_free(&result);

    query_result_init(&result);
    if (assert_true(engine_execute_sql(
                        "DELETE FROM engine_users WHERE name = 'Alice';",
                        &result) == SUCCESS,
                    "engine should execute DELETE successfully") != SUCCESS ||
        assert_true(result.success == 1, "delete result should be successful") != SUCCESS ||
        assert_true(strcmp(result.message, "1 row deleted from engine_users.") == 0,
                    "delete result should contain message") != SUCCESS) {
        query_result_free(&result);
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }
    query_result_free(&result);

    query_result_init(&result);
    if (assert_true(engine_execute_sql(
                        "SELECT * FROM engine_users;",
                        &result) == SUCCESS,
                    "engine should select after delete") != SUCCESS ||
        assert_true(result.row_count == 0, "delete should remove matching rows") != SUCCESS) {
        query_result_free(&result);
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }
    query_result_free(&result);

    query_result_init(&result);
    if (assert_true(engine_execute_sql("SELECT * FROM missing_engine_table;", &result) == FAILURE,
                    "engine should fail missing table SELECT") != SUCCESS ||
        assert_true(result.success == 0, "missing table result should fail") != SUCCESS ||
        assert_true(strstr(result.error, "missing_engine_table") != NULL,
                    "missing table error should mention table name") != SUCCESS) {
        query_result_free(&result);
        table_runtime_cleanup();
        return EXIT_FAILURE;
    }

    query_result_free(&result);
    table_runtime_cleanup();
    puts("[PASS] engine");
    return EXIT_SUCCESS;
}
