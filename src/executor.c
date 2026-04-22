#include "executor.h"

#include "bptree.h"
#include "table_runtime.h"

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

static char *executor_duplicate_cell(const char *value) {
    return utils_strdup(value == NULL ? "" : value);
}

static void executor_set_error(QueryResult *result, const char *message) {
    if (result == NULL) {
        return;
    }

    result->success = 0;
    result->kind = QUERY_RESULT_MESSAGE;
    utils_safe_strcpy(result->error, sizeof(result->error), message);
}

static int executor_allocate_result_rows(QueryResult *result, int row_count) {
    if (result == NULL) {
        return FAILURE;
    }

    if (row_count <= 0) {
        result->rows = NULL;
        result->row_count = 0;
        return SUCCESS;
    }

    result->rows = (char ***)calloc((size_t)row_count, sizeof(char **));
    if (result->rows == NULL) {
        executor_set_error(result, "Failed to allocate result rows.");
        return FAILURE;
    }

    result->row_count = row_count;
    return SUCCESS;
}

static int executor_copy_projected_row(QueryResult *result, int result_index,
                                       char **source_row,
                                       const int *selected_indices) {
    int i;

    result->rows[result_index] = (char **)calloc((size_t)result->column_count, sizeof(char *));
    if (result->rows[result_index] == NULL) {
        executor_set_error(result, "Failed to allocate result row.");
        return FAILURE;
    }

    for (i = 0; i < result->column_count; i++) {
        result->rows[result_index][i] =
            executor_duplicate_cell(source_row[selected_indices[i]]);
        if (result->rows[result_index][i] == NULL) {
            executor_set_error(result, "Failed to duplicate result cell.");
            return FAILURE;
        }
    }

    return SUCCESS;
}

static int executor_prepare_projection(const SelectStatement *stmt,
                                       const TableRuntime *table,
                                       int selected_indices[],
                                       QueryResult *result) {
    int i;
    int column_index;

    if (stmt == NULL || table == NULL || selected_indices == NULL || result == NULL) {
        return FAILURE;
    }

    if (stmt->column_count == 0) {
        result->column_count = table->col_count;
        for (i = 0; i < table->col_count; i++) {
            selected_indices[i] = i;
            if (utils_safe_strcpy(result->columns[i], sizeof(result->columns[i]),
                                  table->columns[i]) != SUCCESS) {
                executor_set_error(result, "Column name is too long.");
                return FAILURE;
            }
        }
        return SUCCESS;
    }

    result->column_count = stmt->column_count;
    for (i = 0; i < stmt->column_count; i++) {
        column_index = executor_find_column_index(table->columns, table->col_count,
                                                  stmt->columns[i]);
        if (column_index == FAILURE) {
            char error[256];
            snprintf(error, sizeof(error), "Column '%s' not found.", stmt->columns[i]);
            executor_set_error(result, error);
            return FAILURE;
        }

        selected_indices[i] = column_index;
        if (utils_safe_strcpy(result->columns[i], sizeof(result->columns[i]),
                              table->columns[column_index]) != SUCCESS) {
            executor_set_error(result, "Column name is too long.");
            return FAILURE;
        }
    }

    return SUCCESS;
}

static int executor_collect_rows_by_indices(const TableRuntime *table,
                                            const int *selected_indices,
                                            const int *row_indices,
                                            int row_count,
                                            QueryResult *result) {
    int i;
    char **source_row;

    if (executor_allocate_result_rows(result, row_count) != SUCCESS) {
        return FAILURE;
    }

    for (i = 0; i < row_count; i++) {
        source_row = table_get_row_by_slot(table, row_indices[i]);
        if (source_row == NULL) {
            executor_set_error(result, "Failed to read result row.");
            return FAILURE;
        }

        if (executor_copy_projected_row(result, i, source_row, selected_indices) != SUCCESS) {
            return FAILURE;
        }
    }

    return SUCCESS;
}

static int executor_collect_all_rows(const TableRuntime *table,
                                     const int *selected_indices,
                                     QueryResult *result) {
    int *row_indices;
    int row_count;
    int status;

    row_indices = NULL;
    row_count = 0;
    if (table_linear_scan_by_field(table, NULL, &row_indices, &row_count) != SUCCESS) {
        executor_set_error(result, "Failed to scan table rows.");
        return FAILURE;
    }

    status = executor_collect_rows_by_indices(table, selected_indices,
                                              row_indices, row_count, result);
    free(row_indices);
    return status;
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

static int executor_collect_rows_by_id(const TableRuntime *table, int search_key,
                                       const int *selected_indices,
                                       QueryResult *result) {
    int row_index;

    row_index = 0;
    if (bptree_search(table->id_index_root, search_key, &row_index) != SUCCESS) {
        result->rows = NULL;
        result->row_count = 0;
        return SUCCESS;
    }

    return executor_collect_rows_by_indices(table, selected_indices,
                                            &row_index, 1, result);
}

static int executor_collect_rows_by_scan(const TableRuntime *table,
                                         const WhereClause *where,
                                         const int *selected_indices,
                                         QueryResult *result) {
    int *row_indices;
    int row_count;
    int status;

    row_indices = NULL;
    row_count = 0;
    if (table_linear_scan_by_field(table, where, &row_indices, &row_count) != SUCCESS) {
        if (result->error[0] == '\0') {
            executor_set_error(result, "Failed to scan rows with WHERE clause.");
        }
        return FAILURE;
    }

    status = executor_collect_rows_by_indices(table, selected_indices,
                                              row_indices, row_count, result);
    free(row_indices);
    return status;
}

static int executor_execute_insert(const InsertStatement *stmt, QueryResult *result) {
    TableRuntimeHandle handle;
    TableRuntime *table;
    int row_index;
    int status;

    memset(&handle, 0, sizeof(handle));
    status = table_runtime_acquire_write(stmt->table_name, &handle);
    if (status != SUCCESS) {
        executor_set_error(result, "Failed to acquire write lock for table.");
        return FAILURE;
    }

    table = table_runtime_handle_table(&handle);
    if (table == NULL) {
        executor_set_error(result, "Failed to access table runtime.");
        table_runtime_release(&handle);
        return FAILURE;
    }

    status = table_insert_row(table, stmt, &row_index);
    table_runtime_release(&handle);
    if (status != SUCCESS) {
        if (result->error[0] == '\0') {
            executor_set_error(result, "Insert failed.");
        }
        return FAILURE;
    }

    result->success = 1;
    result->kind = QUERY_RESULT_MESSAGE;
    snprintf(result->message, sizeof(result->message),
             "1 row inserted into %s.", stmt->table_name);
    return SUCCESS;
}

static int executor_execute_select(const SelectStatement *stmt, QueryResult *result) {
    TableRuntimeHandle handle;
    TableRuntime *table;
    int selected_indices[MAX_COLUMNS];
    int search_key;
    int status;

    memset(&handle, 0, sizeof(handle));
    status = table_runtime_acquire_read(stmt->table_name, &handle);
    if (status != SUCCESS) {
        char error[256];
        snprintf(error, sizeof(error), "Table '%s' not found in runtime.", stmt->table_name);
        executor_set_error(result, error);
        return FAILURE;
    }

    table = table_runtime_handle_table(&handle);
    if (table == NULL || !table->loaded) {
        char error[256];
        snprintf(error, sizeof(error), "Table '%s' not found in runtime.", stmt->table_name);
        executor_set_error(result, error);
        table_runtime_release(&handle);
        return FAILURE;
    }

    result->success = 1;
    result->kind = QUERY_RESULT_TABLE;
    status = executor_prepare_projection(stmt, table, selected_indices, result);
    if (status == SUCCESS) {
        if (!stmt->has_where) {
            status = executor_collect_all_rows(table, selected_indices, result);
        } else if (executor_can_use_id_index(table, &stmt->where, &search_key)) {
            status = executor_collect_rows_by_id(table, search_key, selected_indices, result);
        } else {
            status = executor_collect_rows_by_scan(table, &stmt->where, selected_indices, result);
        }
    }

    table_runtime_release(&handle);
    if (status != SUCCESS) {
        result->success = 0;
        return FAILURE;
    }

    return SUCCESS;
}

static int executor_execute_delete(const DeleteStatement *stmt, QueryResult *result) {
    TableRuntimeHandle handle;
    TableRuntime *table;
    int deleted_count;
    int status;

    memset(&handle, 0, sizeof(handle));
    status = table_runtime_acquire_write(stmt->table_name, &handle);
    if (status != SUCCESS) {
        char error[256];
        snprintf(error, sizeof(error), "Table '%s' not found in runtime.", stmt->table_name);
        executor_set_error(result, error);
        return FAILURE;
    }

    table = table_runtime_handle_table(&handle);
    if (table == NULL || !table->loaded) {
        char error[256];
        snprintf(error, sizeof(error), "Table '%s' not found in runtime.", stmt->table_name);
        executor_set_error(result, error);
        table_runtime_release(&handle);
        return FAILURE;
    }

    status = table_delete_where(table, stmt, &deleted_count);
    table_runtime_release(&handle);
    if (status != SUCCESS) {
        if (result->error[0] == '\0') {
            executor_set_error(result, "Delete failed.");
        }
        return FAILURE;
    }

    result->success = 1;
    result->kind = QUERY_RESULT_MESSAGE;
    snprintf(result->message, sizeof(result->message),
             "%d row%s deleted from %s.",
             deleted_count, deleted_count == 1 ? "" : "s", stmt->table_name);
    return SUCCESS;
}

int executor_execute_to_result(const SqlStatement *statement, QueryResult *result) {
    if (statement == NULL || result == NULL) {
        return FAILURE;
    }

    query_result_init(result);

    switch (statement->type) {
        case SQL_INSERT:
            return executor_execute_insert(&statement->insert, result);
        case SQL_SELECT:
            return executor_execute_select(&statement->select, result);
        case SQL_DELETE:
            return executor_execute_delete(&statement->delete_stmt, result);
        default:
            executor_set_error(result, "Unsupported SQL statement type.");
            return FAILURE;
    }
}

int executor_execute(const SqlStatement *statement) {
    QueryResult result;
    int status;

    query_result_init(&result);
    status = executor_execute_to_result(statement, &result);
    query_result_print(status == SUCCESS ? stdout : stderr, &result);
    query_result_free(&result);
    return status;
}
