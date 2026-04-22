#include "query_result.h"

#include <stdlib.h>
#include <string.h>

void query_result_init(QueryResult *result) {
    if (result == NULL) {
        return;
    }

    memset(result, 0, sizeof(*result));
    result->kind = QUERY_RESULT_MESSAGE;
}

void query_result_free(QueryResult *result) {
    int i;
    int j;

    if (result == NULL) {
        return;
    }

    if (result->rows != NULL) {
        for (i = 0; i < result->row_count; i++) {
            if (result->rows[i] == NULL) {
                continue;
            }

            for (j = 0; j < result->column_count; j++) {
                free(result->rows[i][j]);
                result->rows[i][j] = NULL;
            }

            free(result->rows[i]);
            result->rows[i] = NULL;
        }

        free(result->rows);
    }

    query_result_init(result);
}

int query_result_set_message(QueryResult *result, const char *message) {
    if (result == NULL || message == NULL) {
        return FAILURE;
    }

    result->success = 1;
    result->kind = QUERY_RESULT_MESSAGE;
    result->error[0] = '\0';
    return utils_safe_strcpy(result->message, sizeof(result->message), message);
}

int query_result_set_error(QueryResult *result, const char *error) {
    if (result == NULL || error == NULL) {
        return FAILURE;
    }

    result->success = 0;
    result->kind = QUERY_RESULT_MESSAGE;
    result->message[0] = '\0';
    return utils_safe_strcpy(result->error, sizeof(result->error), error);
}
