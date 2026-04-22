#include "query_result.h"

#include <stdlib.h>
#include <string.h>

static void query_result_free_rows(QueryResult *result) {
    int i;
    int j;

    if (result == NULL || result->rows == NULL) {
        return;
    }

    for (i = 0; i < result->row_count; i++) {
        if (result->rows[i] == NULL) {
            continue;
        }
        for (j = 0; j < result->column_count; j++) {
            free(result->rows[i][j]);
            result->rows[i][j] = NULL;
        }
        free(result->rows[i]);
    }

    free(result->rows);
    result->rows = NULL;
}

void query_result_init(QueryResult *result) {
    if (result == NULL) {
        return;
    }

    memset(result, 0, sizeof(*result));
    result->kind = QUERY_RESULT_NONE;
}

void query_result_free(QueryResult *result) {
    if (result == NULL) {
        return;
    }

    query_result_free_rows(result);
    query_result_init(result);
}

int query_result_set_message(QueryResult *result, const char *message) {
    if (result == NULL || message == NULL) {
        return FAILURE;
    }

    query_result_free_rows(result);
    result->success = 1;
    result->kind = QUERY_RESULT_MESSAGE;
    result->column_count = 0;
    result->row_count = 0;
    result->index_used = 0;
    result->error[0] = '\0';
    return utils_safe_strcpy(result->message, sizeof(result->message), message);
}

int query_result_set_error(QueryResult *result, const char *error) {
    if (result == NULL || error == NULL) {
        return FAILURE;
    }

    query_result_free_rows(result);
    result->success = 0;
    result->kind = QUERY_RESULT_NONE;
    result->message[0] = '\0';
    result->column_count = 0;
    result->row_count = 0;
    result->index_used = 0;
    return utils_safe_strcpy(result->error, sizeof(result->error), error);
}

int query_result_set_table_shape(QueryResult *result,
                                 char columns[][MAX_IDENTIFIER_LEN],
                                 int column_count, int row_count) {
    int i;

    if (result == NULL || columns == NULL || column_count < 0 || row_count < 0) {
        return FAILURE;
    }

    query_result_free_rows(result);
    result->success = 1;
    result->kind = QUERY_RESULT_TABLE;
    result->message[0] = '\0';
    result->error[0] = '\0';
    result->column_count = column_count;
    result->row_count = row_count;

    for (i = 0; i < column_count; i++) {
        if (utils_safe_strcpy(result->columns[i], sizeof(result->columns[i]),
                              columns[i]) != SUCCESS) {
            query_result_set_error(result, "Column name is too long.");
            return FAILURE;
        }
    }

    if (row_count == 0 || column_count == 0) {
        result->rows = NULL;
        return SUCCESS;
    }

    result->rows = (char ***)calloc((size_t)row_count, sizeof(char **));
    if (result->rows == NULL) {
        query_result_set_error(result, "Failed to allocate memory.");
        return FAILURE;
    }

    for (i = 0; i < row_count; i++) {
        result->rows[i] = (char **)calloc((size_t)column_count, sizeof(char *));
        if (result->rows[i] == NULL) {
            query_result_set_error(result, "Failed to allocate memory.");
            return FAILURE;
        }
    }

    return SUCCESS;
}

int query_result_set_cell(QueryResult *result, int row_index, int column_index,
                          const char *value) {
    if (result == NULL || result->rows == NULL || value == NULL ||
        row_index < 0 || row_index >= result->row_count ||
        column_index < 0 || column_index >= result->column_count) {
        return FAILURE;
    }

    result->rows[row_index][column_index] = utils_strdup(value);
    if (result->rows[row_index][column_index] == NULL) {
        query_result_set_error(result, "Failed to allocate memory.");
        return FAILURE;
    }

    return SUCCESS;
}
