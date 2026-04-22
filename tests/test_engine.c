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

static int expect_sql_success(const char *sql, QueryResult *result) {
    int status;

    query_result_init(result);
    status = engine_execute_sql(sql, result);
    if (status != SUCCESS || !result->success) {
        fprintf(stderr, "[FAIL] Expected SQL success: %s\n", sql);
        if (result->error[0] != '\0') {
            fprintf(stderr, "       error: %s\n", result->error);
        }
        query_result_free(result);
        return FAILURE;
    }

    return SUCCESS;
}

static int expect_sql_failure(const char *sql, const char *expected_error) {
    QueryResult result;
    int status;

    query_result_init(&result);
    status = engine_execute_sql(sql, &result);
    if (status != FAILURE || result.success ||
        strstr(result.error, expected_error) == NULL) {
        fprintf(stderr, "[FAIL] Expected SQL failure containing '%s': %s\n",
                expected_error, sql);
        if (result.error[0] != '\0') {
            fprintf(stderr, "       actual error: %s\n", result.error);
        }
        query_result_free(&result);
        return FAILURE;
    }

    query_result_free(&result);
    return SUCCESS;
}

int main(void) {
    QueryResult result;
    char long_identifier[80];
    char long_sql[MAX_SQL_LENGTH + 32];
    char long_value_sql[512];
    char literal[300];
    int i;

    table_runtime_cleanup();

    if (expect_sql_success("INSERT INTO engine_codes (code) VALUES ('100');",
                           &result) != SUCCESS) {
        return EXIT_FAILURE;
    }
    query_result_free(&result);

    if (expect_sql_success("INSERT INTO engine_codes (code) VALUES ('9');",
                           &result) != SUCCESS) {
        return EXIT_FAILURE;
    }
    query_result_free(&result);

    if (expect_sql_success("SELECT code FROM engine_codes WHERE code > '9';",
                           &result) != SUCCESS) {
        return EXIT_FAILURE;
    }
    if (assert_true(result.kind == QUERY_RESULT_TABLE,
                    "string comparison query should return a table result") != SUCCESS ||
        assert_true(result.row_count == 0,
                    "numeric-looking strings should not be compared numerically") != SUCCESS) {
        query_result_free(&result);
        return EXIT_FAILURE;
    }
    query_result_free(&result);

    if (expect_sql_success(
            "INSERT INTO engine_overflow (name, age) VALUES ('Alice', 30);",
            &result) != SUCCESS) {
        return EXIT_FAILURE;
    }
    query_result_free(&result);

    if (expect_sql_failure(
            "SELECT name FROM engine_overflow WHERE age > 999999999999999999999999;",
            "Invalid integer literal for numeric column.") != SUCCESS) {
        return EXIT_FAILURE;
    }

    memset(long_identifier, 'a', sizeof(long_identifier) - 1);
    long_identifier[sizeof(long_identifier) - 1] = '\0';
    snprintf(long_sql, sizeof(long_sql), "SELECT * FROM %s;", long_identifier);
    if (expect_sql_failure(long_sql, "Identifier is too long.") != SUCCESS) {
        return EXIT_FAILURE;
    }

    for (i = 0; i < (int)sizeof(literal) - 1; i++) {
        literal[i] = 'x';
    }
    literal[sizeof(literal) - 1] = '\0';
    snprintf(long_value_sql, sizeof(long_value_sql),
             "INSERT INTO long_values (name) VALUES ('%s');", literal);
    if (expect_sql_failure(long_value_sql, "String literal is too long.") != SUCCESS) {
        return EXIT_FAILURE;
    }

    for (i = 0; i < MAX_SQL_LENGTH + 10; i++) {
        long_sql[i] = 'a';
    }
    long_sql[MAX_SQL_LENGTH + 10] = '\0';
    if (expect_sql_failure(long_sql, "SQL is too long.") != SUCCESS) {
        return EXIT_FAILURE;
    }

    table_runtime_cleanup();
    puts("[PASS] engine");
    return EXIT_SUCCESS;
}
