#include "executor.h"

#include "bptree.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int executor_find_column_index(const char columns[][MAX_IDENTIFIER_LEN],
                                      int col_count, const char *target) {
    int i;

    for (i = 0; i < col_count; i++) {
        if (utils_equals_ignore_case(columns[i], target)) {
            return i;
        }
    }

    return FAILURE;
}

static int executor_prepare_projection(const SelectStatement *stmt,
                                       const TableRuntime *table,
                                       int selected_indices[],
                                       char headers[][MAX_IDENTIFIER_LEN],
                                       int *selected_count,
                                       QueryResult *result) {
    int i;
    int column_index;

    if (stmt == NULL || table == NULL || selected_indices == NULL ||
        headers == NULL || selected_count == NULL || result == NULL) {
        return FAILURE;
    }

    if (stmt->column_count == 0) {
        for (i = 0; i < table->col_count; i++) {
            selected_indices[i] = i;
            if (utils_safe_strcpy(headers[i], sizeof(headers[i]),
                                  table->columns[i]) != SUCCESS) {
                query_result_set_error(result, "Column name is too long.");
                return FAILURE;
            }
        }
        *selected_count = table->col_count;
        return SUCCESS;
    }

    for (i = 0; i < stmt->column_count; i++) {
        column_index = executor_find_column_index(table->columns, table->col_count,
                                                  stmt->columns[i]);
        if (column_index == FAILURE) {
            char error[256];

            snprintf(error, sizeof(error), "Column '%s' not found.", stmt->columns[i]);
            query_result_set_error(result, error);
            return FAILURE;
        }

        selected_indices[i] = column_index;
        if (utils_safe_strcpy(headers[i], sizeof(headers[i]),
                              table->columns[column_index]) != SUCCESS) {
            query_result_set_error(result, "Column name is too long.");
            return FAILURE;
        }
    }

    *selected_count = stmt->column_count;
    return SUCCESS;
}

static int executor_can_use_id_index(const TableRuntime *table,
                                     const WhereClause *where,
                                     int *out_key) {
    long long parsed_key;

    if (table == NULL || where == NULL || out_key == NULL) {
        return 0;
    }

    if (!table->loaded || table->id_index_root == NULL) {
        return 0;
    }

    if (strcmp(where->op, "=") != 0 || !utils_is_integer(where->value)) {
        return 0;
    }

    if (!utils_equals_ignore_case(table->columns[table->id_column_index], where->column)) {
        return 0;
    }

    parsed_key = utils_parse_integer(where->value);
    if (parsed_key < 0 || parsed_key > INT_MAX) {
        return 0;
    }

    *out_key = (int)parsed_key;
    return 1;
}

static int executor_collect_matching_rows(const TableRuntime *table,
                                          const SelectStatement *stmt,
                                          int **out_row_indices,
                                          int *out_row_count,
                                          int *out_index_used,
                                          QueryResult *result) {
    int search_key;
    int row_index;

    *out_row_indices = NULL;
    *out_row_count = 0;
    *out_index_used = 0;

    if (!stmt->has_where) {
        return table_linear_scan_by_field_with_error(table, NULL,
                                                     out_row_indices, out_row_count,
                                                     result->error,
                                                     sizeof(result->error));
    }

    if (executor_can_use_id_index(table, &stmt->where, &search_key)) {
        *out_index_used = 1;
        if (bptree_search(table->id_index_root, search_key, &row_index) != SUCCESS) {
            return SUCCESS;
        }

        *out_row_indices = (int *)malloc(sizeof(int));
        if (*out_row_indices == NULL) {
            query_result_set_error(result, "Failed to allocate memory.");
            return FAILURE;
        }
        (*out_row_indices)[0] = row_index;
        *out_row_count = 1;
        return SUCCESS;
    }

    return table_linear_scan_by_field_with_error(table, &stmt->where,
                                                 out_row_indices, out_row_count,
                                                 result->error,
                                                 sizeof(result->error));
}

static int executor_copy_rows_to_result(const TableRuntime *table,
                                        const int *selected_indices,
                                        int selected_count,
                                        char headers[][MAX_IDENTIFIER_LEN],
                                        const int *row_indices,
                                        int row_count,
                                        int index_used,
                                        QueryResult *result) {
    int i;
    int j;
    char **source_row;

    if (query_result_set_table_shape(result, headers, selected_count, row_count) != SUCCESS) {
        return FAILURE;
    }

    result->index_used = index_used;

    for (i = 0; i < row_count; i++) {
        source_row = table_get_row_by_slot(table, row_indices[i]);
        if (source_row == NULL) {
            query_result_set_error(result, "Failed to read result row.");
            return FAILURE;
        }

        for (j = 0; j < selected_count; j++) {
            if (query_result_set_cell(result, i, j,
                                      source_row[selected_indices[j]] == NULL
                                          ? ""
                                          : source_row[selected_indices[j]]) != SUCCESS) {
                return FAILURE;
            }
        }
    }

    return SUCCESS;
}

static int executor_execute_insert(const InsertStatement *stmt,
                                   TableRuntime *table,
                                   QueryResult *result) {
    int row_index;
    char message[256];

    if (stmt == NULL || table == NULL || result == NULL) {
        return FAILURE;
    }

    if (table_insert_row_with_error(table, stmt, &row_index,
                                    result->error, sizeof(result->error)) != SUCCESS) {
        if (result->error[0] == '\0') {
            query_result_set_error(result, "Failed to insert row.");
        }
        return FAILURE;
    }

    snprintf(message, sizeof(message), "1 row inserted into %s.", stmt->table_name);
    return query_result_set_message(result, message);
}

static int executor_execute_select(const SelectStatement *stmt,
                                   const TableRuntime *table,
                                   QueryResult *result) {
    int selected_indices[MAX_COLUMNS];
    char headers[MAX_COLUMNS][MAX_IDENTIFIER_LEN];
    int selected_count;
    int *row_indices;
    int row_count;
    int index_used;
    int status;

    if (stmt == NULL || table == NULL || result == NULL) {
        return FAILURE;
    }

    if (!table->loaded) {
        char error[256];

        snprintf(error, sizeof(error), "Table '%s' not found.", stmt->table_name);
        query_result_set_error(result, error);
        return FAILURE;
    }

    status = executor_prepare_projection(stmt, table, selected_indices, headers,
                                         &selected_count, result);
    if (status != SUCCESS) {
        return FAILURE;
    }

    row_indices = NULL;
    row_count = 0;
    index_used = 0;
    status = executor_collect_matching_rows(table, stmt, &row_indices, &row_count,
                                            &index_used, result);
    if (status != SUCCESS) {
        if (result->error[0] == '\0') {
            query_result_set_error(result, "Failed to execute SELECT.");
        }
        free(row_indices);
        return FAILURE;
    }

    status = executor_copy_rows_to_result(table, selected_indices, selected_count,
                                          headers, row_indices, row_count,
                                          index_used, result);
    free(row_indices);
    return status;
}

static int executor_execute_delete(QueryResult *result) {
    if (result == NULL) {
        return FAILURE;
    }

    query_result_set_error(result,
                           "DELETE is not supported in memory runtime mode.");
    return FAILURE;
}

int executor_execute_with_runtime(const SqlStatement *statement,
                                  TableRuntime *table,
                                  QueryResult *result) {
    if (statement == NULL || result == NULL) {
        return FAILURE;
    }

    switch (statement->type) {
        case SQL_INSERT:
            return executor_execute_insert(&statement->insert, table, result);
        case SQL_SELECT:
            return executor_execute_select(&statement->select, table, result);
        case SQL_DELETE:
            return executor_execute_delete(result);
        default:
            query_result_set_error(result, "Unsupported SQL statement type.");
            return FAILURE;
    }
}
