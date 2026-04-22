#include "bptree.h"
#include "table_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int assert_true(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "[FAIL] %s\n", message);
        return FAILURE;
    }
    return SUCCESS;
}

static void prepare_insert(InsertStatement *stmt, const char *table_name,
                           int include_id, const char *id, const char *name,
                           const char *age) {
    memset(stmt, 0, sizeof(*stmt));
    snprintf(stmt->table_name, sizeof(stmt->table_name), "%s", table_name);

    if (include_id) {
        stmt->column_count = 3;
        snprintf(stmt->columns[0], sizeof(stmt->columns[0]), "id");
        snprintf(stmt->columns[1], sizeof(stmt->columns[1]), "name");
        snprintf(stmt->columns[2], sizeof(stmt->columns[2]), "age");
        snprintf(stmt->values[0], sizeof(stmt->values[0]), "%s", id);
        snprintf(stmt->values[1], sizeof(stmt->values[1]), "%s", name);
        snprintf(stmt->values[2], sizeof(stmt->values[2]), "%s", age);
        return;
    }

    stmt->column_count = 2;
    snprintf(stmt->columns[0], sizeof(stmt->columns[0]), "name");
    snprintf(stmt->columns[1], sizeof(stmt->columns[1]), "age");
    snprintf(stmt->values[0], sizeof(stmt->values[0]), "%s", name);
    snprintf(stmt->values[1], sizeof(stmt->values[1]), "%s", age);
}

static void prepare_where(WhereClause *where, const char *column, const char *op,
                          const char *value) {
    memset(where, 0, sizeof(*where));
    snprintf(where->column, sizeof(where->column), "%s", column);
    snprintf(where->op, sizeof(where->op), "%s", op);
    snprintf(where->value, sizeof(where->value), "%s", value);
}

int main(void) {
    TableRuntimeHandle users_handle;
    TableRuntimeHandle other_handle;
    TableRuntime *users_table;
    TableRuntime *other_table;
    InsertStatement stmt;
    WhereClause where;
    int row_index;
    int *row_indices;
    int row_count;
    char **row;
    char error[256];
    int registry_before;

    table_runtime_cleanup();
    users_handle.entry = NULL;
    users_handle.lock_mode = TABLE_RUNTIME_LOCK_NONE;
    other_handle.entry = NULL;
    other_handle.lock_mode = TABLE_RUNTIME_LOCK_NONE;

    if (assert_true(table_runtime_acquire_read("runtime_users", &users_handle) ==
                        TABLE_RUNTIME_NOT_FOUND,
                    "read acquire should not create a missing table") != SUCCESS ||
        assert_true(table_runtime_registry_entry_count() == 0,
                    "read acquire miss should not grow the registry") != SUCCESS) {
        return EXIT_FAILURE;
    }

    if (assert_true(table_runtime_acquire_write("runtime_users", &users_handle) == SUCCESS,
                    "write acquire should create runtime_users") != SUCCESS) {
        return EXIT_FAILURE;
    }

    users_table = table_runtime_handle_table(&users_handle);
    if (assert_true(users_table != NULL, "handle should expose runtime_users") != SUCCESS ||
        assert_true(users_table->row_count == 0, "new runtime should start empty") != SUCCESS ||
        assert_true(users_table->next_id == 1, "new runtime should start with next_id 1") != SUCCESS ||
        assert_true(users_table->loaded == 0, "new runtime should start unloaded") != SUCCESS) {
        table_runtime_release(&users_handle);
        return EXIT_FAILURE;
    }

    prepare_insert(&stmt, "runtime_users", 1, "7", "Alice", "30");
    if (assert_true(table_insert_row_with_error(users_table, &stmt, &row_index,
                                                error, sizeof(error)) == FAILURE,
                    "table_insert_row should reject explicit id values") != SUCCESS ||
        assert_true(strstr(error, "Explicit id values are not allowed") != NULL,
                    "explicit id rejection should preserve an error message") != SUCCESS) {
        table_runtime_release(&users_handle);
        return EXIT_FAILURE;
    }

    prepare_insert(&stmt, "runtime_users", 0, "", "Alice", "30");
    if (assert_true(table_insert_row(users_table, &stmt, &row_index) == SUCCESS,
                    "first insert should succeed") != SUCCESS ||
        assert_true(row_index == 0, "first row_index should be 0") != SUCCESS ||
        assert_true(users_table->loaded == 1, "table should become loaded after first insert") != SUCCESS ||
        assert_true(users_table->next_id == 2, "next_id should advance after insert") != SUCCESS) {
        table_runtime_release(&users_handle);
        return EXIT_FAILURE;
    }

    prepare_insert(&stmt, "runtime_users", 0, "", "Bob", "25");
    if (assert_true(table_insert_row(users_table, &stmt, &row_index) == SUCCESS,
                    "second insert should succeed") != SUCCESS ||
        assert_true(row_index == 1, "second row_index should be 1") != SUCCESS ||
        assert_true(users_table->row_count == 2, "runtime should contain two rows") != SUCCESS ||
        assert_true(users_table->next_id == 3, "next_id should advance to 3") != SUCCESS) {
        table_runtime_release(&users_handle);
        return EXIT_FAILURE;
    }

    row = table_get_row_by_slot(users_table, 1);
    if (assert_true(row != NULL, "row lookup by slot should succeed") != SUCCESS ||
        assert_true(strcmp(row[0], "2") == 0, "second row should receive auto id 2") != SUCCESS ||
        assert_true(strcmp(row[1], "Bob") == 0, "second row name should be Bob") != SUCCESS) {
        table_runtime_release(&users_handle);
        return EXIT_FAILURE;
    }

    prepare_where(&where, "age", ">=", "30");
    if (assert_true(table_linear_scan_by_field(users_table, &where, &row_indices, &row_count) ==
                        SUCCESS,
                    "linear scan with WHERE should succeed") != SUCCESS ||
        assert_true(row_count == 1, "only one row should match age >= 30") != SUCCESS ||
        assert_true(row_indices[0] == 0, "Alice should match the age filter") != SUCCESS) {
        free(row_indices);
        table_runtime_release(&users_handle);
        return EXIT_FAILURE;
    }
    free(row_indices);

    if (assert_true(table_linear_scan_by_field(users_table, NULL, &row_indices, &row_count) ==
                        SUCCESS,
                    "full linear scan should succeed") != SUCCESS ||
        assert_true(row_count == 2, "full scan should return all rows") != SUCCESS ||
        assert_true(row_indices[0] == 0 && row_indices[1] == 1,
                    "full scan should preserve row order") != SUCCESS) {
        free(row_indices);
        table_runtime_release(&users_handle);
        return EXIT_FAILURE;
    }
    free(row_indices);

    table_runtime_release(&users_handle);

    registry_before = table_runtime_registry_entry_count();
    if (assert_true(table_runtime_acquire_read("missing_users", &other_handle) ==
                        TABLE_RUNTIME_NOT_FOUND,
                    "missing table reads should continue to fail cleanly") != SUCCESS ||
        assert_true(table_runtime_registry_entry_count() == registry_before,
                    "missing table reads should not add empty registry entries") != SUCCESS) {
        return EXIT_FAILURE;
    }

    if (assert_true(table_runtime_acquire_write("other_users", &other_handle) == SUCCESS,
                    "write acquire should create other_users independently") != SUCCESS) {
        return EXIT_FAILURE;
    }

    other_table = table_runtime_handle_table(&other_handle);
    prepare_insert(&stmt, "other_users", 0, "", "Carol", "29");
    if (assert_true(other_table != NULL, "handle should expose other_users") != SUCCESS ||
        assert_true(table_insert_row(other_table, &stmt, &row_index) == SUCCESS,
                    "insert into other_users should succeed") != SUCCESS ||
        assert_true(other_table->row_count == 1, "other_users should contain one row") != SUCCESS ||
        assert_true(other_table->next_id == 2, "other_users next_id should advance independently") != SUCCESS ||
        assert_true(other_table->id_index_root != NULL,
                    "other_users should build its own id index") != SUCCESS) {
        table_runtime_release(&other_handle);
        return EXIT_FAILURE;
    }

    table_runtime_release(&other_handle);

    if (assert_true(table_runtime_acquire_read("runtime_users", &users_handle) == SUCCESS,
                    "reacquiring runtime_users should preserve its state") != SUCCESS) {
        return EXIT_FAILURE;
    }

    users_table = table_runtime_handle_table(&users_handle);
    if (assert_true(users_table != NULL, "handle should re-expose runtime_users") != SUCCESS ||
        assert_true(users_table->row_count == 2, "runtime_users should still contain two rows") != SUCCESS ||
        assert_true(users_table->next_id == 3, "runtime_users next_id should stay at 3") != SUCCESS ||
        assert_true(users_table->id_index_root != NULL,
                    "runtime_users should keep its own id index") != SUCCESS ||
        assert_true(bptree_search(users_table->id_index_root, 2, &row_index) == SUCCESS,
                    "runtime_users index should still resolve id 2") != SUCCESS ||
        assert_true(row_index == 1, "runtime_users id 2 should still map to row 1") != SUCCESS) {
        table_runtime_release(&users_handle);
        return EXIT_FAILURE;
    }

    table_runtime_release(&users_handle);
    table_runtime_cleanup();
    puts("[PASS] table_runtime");
    return EXIT_SUCCESS;
}
