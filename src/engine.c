#include "engine.h"

#include "engine_error.h"
#include "executor.h"
#include "parser.h"
#include "table_runtime.h"
#include "tokenizer.h"
#include "utils.h"

#include <stdlib.h>

int engine_execute_sql(const char *sql, QueryResult *out_result) {
    Token *tokens;
    int token_count;
    SqlStatement statement;
    char *working_sql;
    int status;
    const char *error_message;

    if (sql == NULL || out_result == NULL) {
        return FAILURE;
    }

    query_result_free(out_result);
    query_result_init(out_result);
    engine_error_clear();

    working_sql = utils_strdup(sql);
    if (working_sql == NULL) {
        engine_error_set("Failed to allocate memory.");
        goto fail;
    }

    utils_trim(working_sql);
    if (working_sql[0] == '\0') {
        free(working_sql);
        out_result->success = 1;
        out_result->kind = QUERY_RESULT_NONE;
        return SUCCESS;
    }

    tokens = tokenizer_tokenize(working_sql, &token_count);
    if (tokens == NULL || token_count == 0) {
        free(tokens);
        free(working_sql);
        if (engine_error_get()[0] == '\0') {
            engine_error_set("Failed to tokenize SQL.");
        }
        goto fail;
    }

    status = parser_parse(tokens, token_count, &statement);
    if (status == SUCCESS) {
        status = executor_execute(&statement, out_result);
    }

    free(tokens);
    free(working_sql);
    if (status != SUCCESS) {
        goto fail;
    }

    return SUCCESS;

fail:
    error_message = engine_error_get();
    query_result_set_error(out_result, error_message);
    return FAILURE;
}

void engine_cleanup(void) {
    table_runtime_cleanup();
    engine_error_clear();
}
