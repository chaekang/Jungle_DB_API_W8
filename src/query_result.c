#include "query_result.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void query_result_print_border(FILE *stream, const int *widths, int col_count) {
    int i;
    int j;

    for (i = 0; i < col_count; i++) {
        fputc('+', stream);
        for (j = 0; j < widths[i] + 2; j++) {
            fputc('-', stream);
        }
    }
    fputs("+\n", stream);
}

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

    if (result == NULL || result->rows == NULL) {
        if (result != NULL) {
            result->row_count = 0;
        }
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
        result->rows[i] = NULL;
    }

    free(result->rows);
    result->rows = NULL;
    result->row_count = 0;
}

void query_result_print(FILE *stream, const QueryResult *result) {
    int widths[MAX_COLUMNS];
    int i;
    int j;
    int cell_width;

    if (stream == NULL || result == NULL) {
        return;
    }

    if (!result->success) {
        fprintf(stream, "%s\n", result->error[0] == '\0' ? "Unknown error." : result->error);
        return;
    }

    if (result->kind == QUERY_RESULT_MESSAGE) {
        fprintf(stream, "%s\n", result->message);
        return;
    }

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
    fprintf(stream, "%d row%s selected.\n", result->row_count,
            result->row_count == 1 ? "" : "s");
}
