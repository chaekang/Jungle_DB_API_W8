#include "executor.h"

#include "bptree.h"
#include "table_runtime.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int executor_fail(QueryResult *result, const char *format, ...) {
    char message[256];
    va_list args;

    if (result == NULL || format == NULL) {
        return FAILURE;
    }

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    query_result_set_error(result, message);
    return FAILURE;
}

static int executor_message(QueryResult *result, const char *format, ...) {
    char message[256];
    va_list args;

    if (result == NULL || format == NULL) {
        return FAILURE;
    }

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    return query_result_set_message(result, message);
}

/*
 * 런타임 스키마에서 컬럼 이름을 대소문자 무시로 찾는다.
 * 컬럼 인덱스를 반환하고, 없으면 FAILURE를 반환한다.
 */
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

static int executor_insert_mentions_id(const InsertStatement *stmt) {
    int i;

    if (stmt == NULL) {
        return 0;
    }

    for (i = 0; i < stmt->column_count; i++) {
        if (utils_equals_ignore_case(stmt->columns[i], "id")) {
            return 1;
        }
    }

    return 0;
}

/*
 * 결과 셀 문자열 하나를 복제한다.
 * NULL 값은 빈 문자열로 처리하며 반환된 메모리는 호출자가 소유한다.
 */
static char *executor_duplicate_cell(const char *value) {
    return utils_strdup(value == NULL ? "" : value);
}

static void executor_free_rows(char ***rows, int row_count, int col_count) {
    int i;
    int j;

    if (rows == NULL) {
        return;
    }

    for (i = 0; i < row_count; i++) {
        if (rows[i] == NULL) {
            continue;
        }

        for (j = 0; j < col_count; j++) {
            free(rows[i][j]);
            rows[i][j] = NULL;
        }
        free(rows[i]);
        rows[i] = NULL;
    }

    free(rows);
}

static int executor_allocate_rows(char ****rows, int row_count) {
    if (rows == NULL) {
        return FAILURE;
    }

    *rows = NULL;
    if (row_count <= 0) {
        return SUCCESS;
    }

    *rows = (char ***)calloc((size_t)row_count, sizeof(char **));
    if (*rows == NULL) {
        fprintf(stderr, "Error: Failed to allocate memory.\n");
        return FAILURE;
    }

    return SUCCESS;
}

/*
 * SELECT 대상 컬럼을 런타임 테이블 인덱스와 결과 헤더로 변환한다.
 */
static int executor_prepare_projection(const SelectStatement *stmt,
                                       const TableRuntime *table,
                                       int selected_indices[],
                                       QueryResult *result) {
    int i;
    int column_index;

    if (stmt == NULL || table == NULL || selected_indices == NULL ||
        result == NULL) {
        return FAILURE;
    }

    result->kind = QUERY_RESULT_TABLE;
    result->column_count = 0;

    if (stmt->column_count == 0) {
        for (i = 0; i < table->col_count; i++) {
            selected_indices[i] = i;
            if (utils_safe_strcpy(result->columns[i], sizeof(result->columns[i]),
                                  table->columns[i]) != SUCCESS) {
                return executor_fail(result, "Column name is too long.");
            }
        }
        result->column_count = table->col_count;
        return SUCCESS;
    }

    for (i = 0; i < stmt->column_count; i++) {
        column_index = executor_find_column_index(table->columns, table->col_count,
                                                  stmt->columns[i]);
        if (column_index == FAILURE) {
            return executor_fail(result, "Column '%s' not found.", stmt->columns[i]);
        }

        selected_indices[i] = column_index;
        if (utils_safe_strcpy(result->columns[i], sizeof(result->columns[i]),
                              table->columns[column_index]) != SUCCESS) {
            return executor_fail(result, "Column name is too long.");
        }
    }

    result->column_count = stmt->column_count;
    return SUCCESS;
}

static int executor_copy_projected_row(char ***rows, int result_index,
                                       char **source_row,
                                       const int *selected_indices,
                                       int selected_count) {
    int i;

    rows[result_index] = (char **)calloc((size_t)selected_count, sizeof(char *));
    if (rows[result_index] == NULL) {
        fprintf(stderr, "Error: Failed to allocate memory.\n");
        return FAILURE;
    }

    for (i = 0; i < selected_count; i++) {
        rows[result_index][i] =
            executor_duplicate_cell(source_row[selected_indices[i]]);
        if (rows[result_index][i] == NULL) {
            return FAILURE;
        }
    }

    return SUCCESS;
}

/*
 * row_index 목록을 실제 결과 행 배열로 바꿔 QueryResult에 복사한다.
 */
static int executor_collect_rows_by_indices(const TableRuntime *table,
                                            const int *selected_indices,
                                            int selected_count,
                                            const int *row_indices,
                                            int row_count,
                                            QueryResult *result) {
    char ***rows;
    char **source_row;
    int i;

    if (table == NULL || selected_indices == NULL || result == NULL) {
        return FAILURE;
    }

    if (executor_allocate_rows(&rows, row_count) != SUCCESS) {
        return executor_fail(result, "Failed to allocate result rows.");
    }

    for (i = 0; i < row_count; i++) {
        source_row = table_get_row_by_slot(table, row_indices[i]);
        if (source_row == NULL ||
            executor_copy_projected_row(rows, i, source_row,
                                        selected_indices,
                                        selected_count) != SUCCESS) {
            executor_free_rows(rows, row_count, selected_count);
            return executor_fail(result, "Failed to copy result rows.");
        }
    }

    result->success = 1;
    result->kind = QUERY_RESULT_TABLE;
    result->rows = rows;
    result->row_count = row_count;
    result->message[0] = '\0';
    result->error[0] = '\0';
    return SUCCESS;
}

static int executor_collect_all_rows(const TableRuntime *table,
                                     const int *selected_indices,
                                     int selected_count,
                                     QueryResult *result) {
    int *row_indices;
    int row_count;
    int status;

    row_indices = NULL;
    row_count = 0;
    if (table_linear_scan_by_field(table, NULL, &row_indices, &row_count) != SUCCESS) {
        return executor_fail(result, "%s", table_runtime_get_last_error());
    }

    status = executor_collect_rows_by_indices(table, selected_indices, selected_count,
                                              row_indices, row_count, result);
    free(row_indices);
    return status;
}

/*
 * WHERE가 id 등호 비교면 B+ 트리를 사용할 수 있는지 확인한다.
 */
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

    if (!utils_equals_ignore_case(table->columns[table->id_column_index],
                                  where->column)) {
        return 0;
    }

    if (utils_try_parse_integer(where->value, &parsed_key) != SUCCESS) {
        return 0;
    }
    if (parsed_key < 0 || parsed_key > INT_MAX) {
        return 0;
    }

    *out_key = (int)parsed_key;
    return 1;
}

static int executor_collect_rows_by_id(const TableRuntime *table, int search_key,
                                       const int *selected_indices,
                                       int selected_count,
                                       QueryResult *result) {
    int row_index;

    row_index = 0;
    if (bptree_search(table->id_index_root, search_key, &row_index) != SUCCESS) {
        return executor_collect_rows_by_indices(table, selected_indices,
                                                selected_count, NULL, 0, result);
    }

    return executor_collect_rows_by_indices(table, selected_indices, selected_count,
                                            &row_index, 1, result);
}

static int executor_collect_rows_by_scan(const TableRuntime *table,
                                         const WhereClause *where,
                                         const int *selected_indices,
                                         int selected_count,
                                         QueryResult *result) {
    int *row_indices;
    int row_count;
    int status;

    row_indices = NULL;
    row_count = 0;
    if (table_linear_scan_by_field(table, where, &row_indices, &row_count) != SUCCESS) {
        return executor_fail(result, "%s", table_runtime_get_last_error());
    }

    status = executor_collect_rows_by_indices(table, selected_indices, selected_count,
                                              row_indices, row_count, result);
    free(row_indices);
    return status;
}

static int executor_execute_insert(const InsertStatement *stmt,
                                   QueryResult *result) {
    TableRuntimeHandle handle;
    TableRuntime *table;
    int row_index;
    int status;

    if (stmt == NULL) {
        return executor_fail(result, "INSERT statement is empty.");
    }

    if (executor_insert_mentions_id(stmt)) {
        return executor_fail(result, "Explicit id values are not allowed.");
    }

    handle.entry = NULL;
    if (table_runtime_acquire_write(stmt->table_name, &handle) != SUCCESS) {
        return executor_fail(result, "Failed to open table '%s' for writing.",
                             stmt->table_name);
    }

    table = table_runtime_handle_table(&handle);
    if (table == NULL) {
        table_runtime_release(&handle);
        return executor_fail(result, "Failed to open table '%s'.", stmt->table_name);
    }

    status = table_insert_row(table, stmt, &row_index);
    table_runtime_release(&handle);
    if (status != SUCCESS) {
        return executor_fail(result, "%s", table_runtime_get_last_error());
    }

    (void)row_index;
    return executor_message(result, "1 row inserted into %s.", stmt->table_name);
}

static int executor_execute_select(const SelectStatement *stmt,
                                   QueryResult *result) {
    TableRuntimeHandle handle;
    TableRuntime *table;
    int selected_indices[MAX_COLUMNS];
    int search_key;
    int status;

    if (stmt == NULL) {
        return executor_fail(result, "SELECT statement is empty.");
    }

    handle.entry = NULL;
    if (table_runtime_acquire_read(stmt->table_name, &handle) != SUCCESS) {
        return executor_fail(result, "Table '%s' not found.", stmt->table_name);
    }

    table = table_runtime_handle_table(&handle);
    if (table == NULL || !table->loaded) {
        table_runtime_release(&handle);
        return executor_fail(result, "Table '%s' not found.", stmt->table_name);
    }

    if (executor_prepare_projection(stmt, table, selected_indices,
                                    result) != SUCCESS) {
        table_runtime_release(&handle);
        return FAILURE;
    }

    if (!stmt->has_where) {
        status = executor_collect_all_rows(table, selected_indices,
                                           result->column_count, result);
    } else if (executor_can_use_id_index(table, &stmt->where, &search_key)) {
        status = executor_collect_rows_by_id(table, search_key, selected_indices,
                                             result->column_count, result);
    } else {
        status = executor_collect_rows_by_scan(table, &stmt->where,
                                               selected_indices,
                                               result->column_count, result);
    }

    table_runtime_release(&handle);
    return status;
}

static int executor_execute_delete(const DeleteStatement *stmt,
                                   QueryResult *result) {
    (void)stmt;
    return executor_fail(result,
                         "DELETE is not supported in memory runtime mode.");
}

int executor_execute(const SqlStatement *statement, QueryResult *out_result) {
    if (out_result == NULL) {
        return FAILURE;
    }

    query_result_init(out_result);
    if (statement == NULL) {
        return executor_fail(out_result, "SQL statement is empty.");
    }

    switch (statement->type) {
        case SQL_INSERT:
            return executor_execute_insert(&statement->insert, out_result);
        case SQL_SELECT:
            return executor_execute_select(&statement->select, out_result);
        case SQL_DELETE:
            return executor_execute_delete(&statement->delete_stmt, out_result);
        default:
            return executor_fail(out_result, "Unsupported SQL statement type.");
    }
}
