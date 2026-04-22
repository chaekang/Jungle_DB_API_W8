#include "engine.h"

#include "executor.h"
#include "parser.h"
#include "table_runtime.h"
#include "tokenizer.h"

#include <stdio.h>
#include <stdlib.h>

static int engine_set_default_error(QueryResult *result, const char *fallback) {
    if (result->error[0] != '\0') {
        return FAILURE;
    }

    query_result_set_error(result, fallback);
    return FAILURE;
}

int engine_execute_sql(const char *sql, QueryResult *out_result) {
    Token *tokens;
    int token_count;
    SqlStatement statement;
    TableRuntimeHandle handle;
    TableRuntime *table;
    int status;

    if (sql == NULL || out_result == NULL) {
        return FAILURE;
    }

    query_result_init(out_result);
    handle.entry = NULL;
    handle.lock_mode = TABLE_RUNTIME_LOCK_NONE;

    tokens = tokenizer_tokenize_with_error(sql, &token_count,
                                           out_result->error,
                                           sizeof(out_result->error));
    if (tokens == NULL) {
        return engine_set_default_error(out_result, "Failed to tokenize SQL.");
    }

    status = parser_parse_with_error(tokens, token_count, &statement,
                                     out_result->error,
                                     sizeof(out_result->error));
    free(tokens);
    if (status != SUCCESS) {
        return engine_set_default_error(out_result, "Failed to parse SQL.");
    }

    if (statement.type == SQL_SELECT) {
        status = table_runtime_acquire_read(statement.select.table_name, &handle);
        if (status == TABLE_RUNTIME_NOT_FOUND) {
            char error[256];

            snprintf(error, sizeof(error), "Table '%s' not found.",
                     statement.select.table_name);
            query_result_set_error(out_result, error);
            return FAILURE;
        }
    } else if (statement.type == SQL_INSERT) {
        status = table_runtime_acquire_write(statement.insert.table_name, &handle);
    } else {
        status = SUCCESS;
    }

    if (status != SUCCESS && status != TABLE_RUNTIME_NOT_FOUND) {
        query_result_set_error(out_result, "Failed to acquire table runtime.");
        return FAILURE;
    }

    table = table_runtime_handle_table(&handle);
    status = executor_execute_with_runtime(&statement, table, out_result);
    table_runtime_release(&handle);

    if (status != SUCCESS) {
        return engine_set_default_error(out_result, "Engine execution failed.");
    }

    return SUCCESS;
}
