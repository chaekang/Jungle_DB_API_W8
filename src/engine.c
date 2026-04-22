#include "engine.h"

#include "executor.h"
#include "parser.h"
#include "table_runtime.h"
#include "tokenizer.h"

#include <stdlib.h>
#include <string.h>

int engine_execute_sql(const char *sql, QueryResult *out_result) {
    Token *tokens;
    int token_count;
    SqlStatement statement;
    int status;

    if (out_result == NULL) {
        return FAILURE;
    }

    query_result_init(out_result);
    if (sql == NULL) {
        query_result_set_error(out_result, "SQL is empty.");
        return FAILURE;
    }

    if (strlen(sql) >= MAX_SQL_LENGTH) {
        query_result_set_error(out_result, "SQL is too long.");
        return FAILURE;
    }

    tokens = tokenizer_tokenize(sql, &token_count);
    if (tokens == NULL || token_count == 0) {
        free(tokens);
        query_result_set_error(out_result, tokenizer_get_last_error());
        return FAILURE;
    }

    status = parser_parse(tokens, token_count, &statement);
    free(tokens);
    if (status != SUCCESS) {
        query_result_set_error(out_result, parser_get_last_error());
        return FAILURE;
    }

    return executor_execute(&statement, out_result);
}

void engine_shutdown(void) {
    table_runtime_cleanup();
    tokenizer_cleanup_cache();
}
