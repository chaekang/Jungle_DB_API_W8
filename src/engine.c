#include "engine.h"

#include "executor.h"
#include "parser.h"
#include "tokenizer.h"
#include "utils.h"

#include <stdlib.h>

int engine_execute_sql(const char *sql, QueryResult *out_result) {
    Token *tokens;
    int token_count;
    SqlStatement statement;
    char *working_sql;
    int status;

    if (sql == NULL || out_result == NULL) {
        return FAILURE;
    }

    query_result_init(out_result);
    working_sql = utils_strdup(sql);
    if (working_sql == NULL) {
        utils_safe_strcpy(out_result->error, sizeof(out_result->error),
                          "Failed to allocate SQL buffer.");
        return FAILURE;
    }

    utils_trim(working_sql);
    if (working_sql[0] == '\0') {
        free(working_sql);
        utils_safe_strcpy(out_result->error, sizeof(out_result->error),
                          "SQL query is empty.");
        return FAILURE;
    }

    tokens = tokenizer_tokenize(working_sql, &token_count);
    free(working_sql);
    if (tokens == NULL || token_count == 0) {
        free(tokens);
        utils_safe_strcpy(out_result->error, sizeof(out_result->error),
                          "Failed to tokenize SQL.");
        return FAILURE;
    }

    status = parser_parse(tokens, token_count, &statement);
    free(tokens);
    if (status != SUCCESS) {
        utils_safe_strcpy(out_result->error, sizeof(out_result->error),
                          "Failed to parse SQL.");
        return FAILURE;
    }

    return executor_execute_to_result(&statement, out_result);
}
