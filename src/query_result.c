#include "query_result.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void query_result_free_rows(char ***rows, int row_count, int column_count) {
    int i;
    int j;

    if (rows == NULL) {
        return;
    }

    for (i = 0; i < row_count; i++) {
        if (rows[i] == NULL) {
            continue;
        }
        for (j = 0; j < column_count; j++) {
            free(rows[i][j]);
            rows[i][j] = NULL;
        }
        free(rows[i]);
    }

    free(rows);
}

static void query_result_print_border(FILE *stream, const int *widths, int column_count) {
    int i;
    int j;

    for (i = 0; i < column_count; i++) {
        fputc('+', stream);
        for (j = 0; j < widths[i] + 2; j++) {
            fputc('-', stream);
        }
    }
    fputs("+\n", stream);
}

static void query_result_print_table(FILE *stream, const QueryResult *result) {
    int widths[MAX_COLUMNS];
    int i;
    int j;
    int cell_width;

    for (i = 0; i < result->column_count; i++) {
        widths[i] = utils_display_width(result->columns[i]);
    }

    for (i = 0; i < result->row_count; i++) {
        for (j = 0; j < result->column_count; j++) {
            cell_width = utils_display_width(result->rows[i][j]);
            if (cell_width > widths[j]) {
                widths[j] = cell_width;
            }
        }
    }

    query_result_print_border(stream, widths, result->column_count);
    for (i = 0; i < result->column_count; i++) {
        fputs("| ", stream);
        utils_print_padded(stream, result->columns[i], widths[i]);
        fputc(' ', stream);
    }
    fputs("|\n", stream);
    query_result_print_border(stream, widths, result->column_count);

    for (i = 0; i < result->row_count; i++) {
        for (j = 0; j < result->column_count; j++) {
            fputs("| ", stream);
            utils_print_padded(stream, result->rows[i][j], widths[j]);
            fputc(' ', stream);
        }
        fputs("|\n", stream);
    }

    query_result_print_border(stream, widths, result->column_count);
}

void query_result_init(QueryResult *result) {
    if (result == NULL) {
        return;
    }

    memset(result, 0, sizeof(*result));
}

void query_result_free(QueryResult *result) {
    if (result == NULL) {
        return;
    }

    query_result_free_rows(result->rows, result->row_count, result->column_count);
    query_result_init(result);
}

int query_result_set_message(QueryResult *result, const char *message) {
    if (result == NULL || message == NULL) {
        return FAILURE;
    }

    result->success = 1;
    result->kind = QUERY_RESULT_MESSAGE;
    if (utils_safe_strcpy(result->message, sizeof(result->message), message) != SUCCESS) {
        return FAILURE;
    }
    return SUCCESS;
}

int query_result_set_error(QueryResult *result, const char *error) {
    const char *safe_error;

    if (result == NULL) {
        return FAILURE;
    }

    safe_error = (error == NULL || error[0] == '\0')
                     ? "Unexpected DB engine error."
                     : error;
    result->success = 0;
    result->kind = QUERY_RESULT_NONE;
    if (utils_safe_strcpy(result->error, sizeof(result->error), safe_error) != SUCCESS) {
        return FAILURE;
    }
    return SUCCESS;
}

int query_result_take_table(QueryResult *result,
                            const char columns[][MAX_IDENTIFIER_LEN],
                            int column_count,
                            char ***rows,
                            int row_count) {
    int i;

    if (result == NULL || columns == NULL || column_count <= 0 || column_count > MAX_COLUMNS) {
        query_result_free_rows(rows, row_count, column_count);
        return FAILURE;
    }

    result->success = 1;
    result->kind = QUERY_RESULT_TABLE;
    result->column_count = column_count;
    result->rows = rows;
    result->row_count = row_count;

    for (i = 0; i < column_count; i++) {
        if (utils_safe_strcpy(result->columns[i], sizeof(result->columns[i]), columns[i]) != SUCCESS) {
            query_result_free(result);
            return FAILURE;
        }
    }

    return SUCCESS;
}

void query_result_print_text(FILE *stream, const QueryResult *result) {
    if (stream == NULL || result == NULL || !result->success) {
        return;
    }

    if (result->kind == QUERY_RESULT_MESSAGE) {
        fprintf(stream, "%s\n", result->message);
        return;
    }

    if (result->kind == QUERY_RESULT_TABLE) {
        query_result_print_table(stream, result);
        fprintf(stream, "%d row%s selected.\n",
                result->row_count,
                result->row_count == 1 ? "" : "s");
    }
}
