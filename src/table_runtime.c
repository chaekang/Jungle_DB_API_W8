#include "table_runtime.h"

#include "bptree.h"
#include "utils.h"

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct TableRuntimeEntry {
    TableRuntime table;
    pthread_rwlock_t lock;
    struct TableRuntimeEntry *next;
} TableRuntimeEntry;

static TableRuntimeEntry *table_runtime_registry_head = NULL;
static pthread_mutex_t table_runtime_registry_lock = PTHREAD_MUTEX_INITIALIZER;

static void table_free_owned_row(char **row, int col_count);
static int table_find_column_index(const TableRuntime *table, const char *target);
static int table_validate_insert_schema(const TableRuntime *table,
                                        const InsertStatement *stmt,
                                        char *error, size_t error_size);
static int table_initialize_schema(TableRuntime *table,
                                   const InsertStatement *stmt,
                                   char *error, size_t error_size);
static char **table_build_row(const TableRuntime *table, const InsertStatement *stmt,
                              long long next_id, char *error, size_t error_size);
static int table_where_matches(int comparison, const char *op);
static TableRuntimeEntry *table_runtime_find_entry_locked(const char *table_name);
static TableRuntimeEntry *table_runtime_create_entry_locked(const char *table_name);
static void table_runtime_destroy_entry(TableRuntimeEntry *entry);

static void table_free_owned_row(char **row, int col_count) {
    int i;

    if (row == NULL) {
        return;
    }

    for (i = 0; i < col_count; i++) {
        free(row[i]);
        row[i] = NULL;
    }
    free(row);
}

static int table_find_column_index(const TableRuntime *table, const char *target) {
    int i;

    if (table == NULL || target == NULL) {
        return FAILURE;
    }

    for (i = 0; i < table->col_count; i++) {
        if (utils_equals_ignore_case(table->columns[i], target)) {
            return i;
        }
    }

    return FAILURE;
}

static int table_validate_insert_schema(const TableRuntime *table,
                                        const InsertStatement *stmt,
                                        char *error, size_t error_size) {
    int i;

    if (table == NULL || stmt == NULL) {
        return FAILURE;
    }

    for (i = 0; i < stmt->column_count; i++) {
        if (utils_equals_ignore_case(stmt->columns[i], "id")) {
            utils_set_error(error, error_size, "Explicit id values are not allowed.");
            return FAILURE;
        }
    }

    if (stmt->column_count <= 0 || stmt->column_count != table->col_count - 1) {
        utils_set_error(error, error_size,
                        "INSERT columns do not match table schema.");
        return FAILURE;
    }

    for (i = 0; i < stmt->column_count; i++) {
        if (!utils_equals_ignore_case(table->columns[i + 1], stmt->columns[i])) {
            utils_set_error(error, error_size,
                            "INSERT columns do not match table schema.");
            return FAILURE;
        }
    }

    return SUCCESS;
}

static int table_initialize_schema(TableRuntime *table, const InsertStatement *stmt,
                                   char *error, size_t error_size) {
    int i;

    if (table == NULL || stmt == NULL) {
        return FAILURE;
    }

    if (stmt->column_count <= 0 || stmt->column_count + 1 > MAX_COLUMNS) {
        utils_set_error(error, error_size, "Invalid INSERT column count.");
        return FAILURE;
    }

    table->col_count = stmt->column_count + 1;
    if (utils_safe_strcpy(table->columns[0], sizeof(table->columns[0]), "id") != SUCCESS) {
        utils_set_error(error, error_size, "Column name is too long.");
        return FAILURE;
    }

    for (i = 0; i < stmt->column_count; i++) {
        if (utils_equals_ignore_case(stmt->columns[i], "id")) {
            utils_set_error(error, error_size, "Explicit id values are not allowed.");
            return FAILURE;
        }

        if (utils_safe_strcpy(table->columns[i + 1], sizeof(table->columns[i + 1]),
                              stmt->columns[i]) != SUCCESS) {
            utils_set_error(error, error_size, "Column name is too long.");
            return FAILURE;
        }
    }

    table->id_column_index = 0;
    table->loaded = 1;
    return SUCCESS;
}

static char **table_build_row(const TableRuntime *table, const InsertStatement *stmt,
                              long long next_id, char *error, size_t error_size) {
    char **row;
    char id_buffer[MAX_VALUE_LEN];
    int i;

    if (table == NULL || stmt == NULL) {
        return NULL;
    }

    row = (char **)calloc((size_t)table->col_count, sizeof(char *));
    if (row == NULL) {
        utils_set_error(error, error_size, "Failed to allocate memory.");
        return NULL;
    }

    snprintf(id_buffer, sizeof(id_buffer), "%lld", next_id);
    row[0] = utils_strdup(id_buffer);
    if (row[0] == NULL) {
        free(row);
        utils_set_error(error, error_size, "Failed to allocate memory.");
        return NULL;
    }

    for (i = 0; i < stmt->column_count; i++) {
        row[i + 1] = utils_strdup(stmt->values[i]);
        if (row[i + 1] == NULL) {
            int j;

            for (j = 0; j <= i; j++) {
                free(row[j]);
            }
            free(row);
            utils_set_error(error, error_size, "Failed to allocate memory.");
            return NULL;
        }
    }

    return row;
}

static int table_where_matches(int comparison, const char *op) {
    if (strcmp(op, "=") == 0) {
        return comparison == 0;
    }
    if (strcmp(op, "!=") == 0) {
        return comparison != 0;
    }
    if (strcmp(op, ">") == 0) {
        return comparison > 0;
    }
    if (strcmp(op, ">=") == 0) {
        return comparison >= 0;
    }
    if (strcmp(op, "<") == 0) {
        return comparison < 0;
    }
    if (strcmp(op, "<=") == 0) {
        return comparison <= 0;
    }

    return 0;
}

static TableRuntimeEntry *table_runtime_find_entry_locked(const char *table_name) {
    TableRuntimeEntry *entry;

    entry = table_runtime_registry_head;
    while (entry != NULL) {
        if (utils_equals_ignore_case(entry->table.table_name, table_name)) {
            return entry;
        }
        entry = entry->next;
    }

    return NULL;
}

static TableRuntimeEntry *table_runtime_create_entry_locked(const char *table_name) {
    TableRuntimeEntry *entry;

    entry = (TableRuntimeEntry *)calloc(1, sizeof(*entry));
    if (entry == NULL) {
        return NULL;
    }

    table_init(&entry->table);
    if (utils_safe_strcpy(entry->table.table_name, sizeof(entry->table.table_name),
                          table_name) != SUCCESS) {
        free(entry);
        return NULL;
    }

    if (pthread_rwlock_init(&entry->lock, NULL) != 0) {
        free(entry);
        return NULL;
    }

    entry->next = table_runtime_registry_head;
    table_runtime_registry_head = entry;
    return entry;
}

static void table_runtime_destroy_entry(TableRuntimeEntry *entry) {
    if (entry == NULL) {
        return;
    }

    table_free(&entry->table);
    pthread_rwlock_destroy(&entry->lock);
    free(entry);
}

void table_init(TableRuntime *table) {
    if (table == NULL) {
        return;
    }

    memset(table, 0, sizeof(*table));
    table->id_column_index = 0;
    table->next_id = 1;
}

void table_free(TableRuntime *table) {
    int i;

    if (table == NULL) {
        return;
    }

    bptree_free(table->id_index_root);
    table->id_index_root = NULL;

    if (table->rows != NULL) {
        for (i = 0; i < table->row_count; i++) {
            table_free_owned_row(table->rows[i], table->col_count);
            table->rows[i] = NULL;
        }
        free(table->rows);
        table->rows = NULL;
    }

    table_init(table);
}

int table_reserve_if_needed(TableRuntime *table) {
    char ***new_rows;
    int new_capacity;

    if (table == NULL) {
        return FAILURE;
    }

    if (table->row_count < table->capacity) {
        return SUCCESS;
    }

    new_capacity = table->capacity == 0 ? INITIAL_ROW_CAPACITY : table->capacity * 2;
    new_rows = (char ***)realloc(table->rows, (size_t)new_capacity * sizeof(char **));
    if (new_rows == NULL) {
        return FAILURE;
    }

    table->rows = new_rows;
    table->capacity = new_capacity;
    return SUCCESS;
}

static int table_runtime_acquire_internal(const char *table_name,
                                          TableRuntimeHandle *out_handle,
                                          TableRuntimeLockMode mode) {
    TableRuntimeEntry *entry;

    if (table_name == NULL || out_handle == NULL) {
        return FAILURE;
    }

    out_handle->entry = NULL;
    out_handle->lock_mode = TABLE_RUNTIME_LOCK_NONE;

    if (table_name[0] == '\0') {
        return FAILURE;
    }

    if (pthread_mutex_lock(&table_runtime_registry_lock) != 0) {
        return FAILURE;
    }

    entry = table_runtime_find_entry_locked(table_name);
    if (entry == NULL && mode == TABLE_RUNTIME_LOCK_WRITE) {
        entry = table_runtime_create_entry_locked(table_name);
    }

    if (entry == NULL) {
        pthread_mutex_unlock(&table_runtime_registry_lock);
        return mode == TABLE_RUNTIME_LOCK_READ ? TABLE_RUNTIME_NOT_FOUND : FAILURE;
    }

    if (mode == TABLE_RUNTIME_LOCK_READ) {
        if (pthread_rwlock_rdlock(&entry->lock) != 0) {
            pthread_mutex_unlock(&table_runtime_registry_lock);
            return FAILURE;
        }
    } else {
        if (pthread_rwlock_wrlock(&entry->lock) != 0) {
            pthread_mutex_unlock(&table_runtime_registry_lock);
            return FAILURE;
        }
    }

    pthread_mutex_unlock(&table_runtime_registry_lock);

    out_handle->entry = entry;
    out_handle->lock_mode = mode;
    return SUCCESS;
}

int table_runtime_acquire_read(const char *table_name, TableRuntimeHandle *out_handle) {
    return table_runtime_acquire_internal(table_name, out_handle,
                                          TABLE_RUNTIME_LOCK_READ);
}

int table_runtime_acquire_write(const char *table_name, TableRuntimeHandle *out_handle) {
    return table_runtime_acquire_internal(table_name, out_handle,
                                          TABLE_RUNTIME_LOCK_WRITE);
}

int table_runtime_acquire(const char *table_name, TableRuntimeHandle *out_handle) {
    return table_runtime_acquire_write(table_name, out_handle);
}

TableRuntime *table_runtime_handle_table(TableRuntimeHandle *handle) {
    if (handle == NULL || handle->entry == NULL) {
        return NULL;
    }

    return &handle->entry->table;
}

void table_runtime_release(TableRuntimeHandle *handle) {
    if (handle == NULL || handle->entry == NULL) {
        return;
    }

    if (handle->lock_mode == TABLE_RUNTIME_LOCK_READ) {
        pthread_rwlock_unlock(&handle->entry->lock);
    } else if (handle->lock_mode == TABLE_RUNTIME_LOCK_WRITE) {
        pthread_rwlock_unlock(&handle->entry->lock);
    }

    handle->entry = NULL;
    handle->lock_mode = TABLE_RUNTIME_LOCK_NONE;
}

int table_runtime_registry_entry_count(void) {
    TableRuntimeEntry *entry;
    int count;

    if (pthread_mutex_lock(&table_runtime_registry_lock) != 0) {
        return FAILURE;
    }

    count = 0;
    entry = table_runtime_registry_head;
    while (entry != NULL) {
        count++;
        entry = entry->next;
    }

    pthread_mutex_unlock(&table_runtime_registry_lock);
    return count;
}

int table_insert_row_with_error(TableRuntime *table, const InsertStatement *stmt,
                                int *out_row_index, char *error,
                                size_t error_size) {
    char **row;
    int row_index;

    if (table == NULL || stmt == NULL || out_row_index == NULL) {
        return FAILURE;
    }

    if (table->table_name[0] == '\0') {
        if (utils_safe_strcpy(table->table_name, sizeof(table->table_name),
                              stmt->table_name) != SUCCESS) {
            utils_set_error(error, error_size, "Table name is too long.");
            return FAILURE;
        }
    }

    if (!utils_equals_ignore_case(table->table_name, stmt->table_name)) {
        utils_set_error(error, error_size,
                        "Active runtime table does not match INSERT target.");
        return FAILURE;
    }

    if (!table->loaded) {
        if (table_initialize_schema(table, stmt, error, error_size) != SUCCESS) {
            return FAILURE;
        }
    } else if (table_validate_insert_schema(table, stmt, error, error_size) != SUCCESS) {
        return FAILURE;
    }

    if (table_reserve_if_needed(table) != SUCCESS) {
        utils_set_error(error, error_size, "Failed to allocate memory.");
        return FAILURE;
    }

    row = table_build_row(table, stmt, table->next_id, error, error_size);
    if (row == NULL) {
        return FAILURE;
    }

    if (table->next_id > INT_MAX) {
        utils_set_error(error, error_size, "Runtime id exceeds B+ tree key range.");
        table_free_owned_row(row, table->col_count);
        return FAILURE;
    }

    row_index = table->row_count;
    table->rows[row_index] = row;
    if (bptree_insert(&table->id_index_root, (int)table->next_id, row_index) != SUCCESS) {
        utils_set_error(error, error_size, "Failed to update id index.");
        table_free_owned_row(table->rows[row_index], table->col_count);
        table->rows[row_index] = NULL;
        return FAILURE;
    }

    *out_row_index = row_index;
    table->row_count++;
    table->next_id++;
    return SUCCESS;
}

int table_insert_row(TableRuntime *table, const InsertStatement *stmt,
                     int *out_row_index) {
    return table_insert_row_with_error(table, stmt, out_row_index, NULL, 0);
}

char **table_get_row_by_slot(const TableRuntime *table, int row_index) {
    if (table == NULL || row_index < 0 || row_index >= table->row_count) {
        return NULL;
    }

    return table->rows[row_index];
}

int table_linear_scan_by_field_with_error(const TableRuntime *table,
                                          const WhereClause *where,
                                          int **out_row_indices, int *out_count,
                                          char *error, size_t error_size) {
    int *matches;
    int match_count;
    int i;
    int where_column_index;

    if (table == NULL || out_row_indices == NULL || out_count == NULL) {
        return FAILURE;
    }

    *out_row_indices = NULL;
    *out_count = 0;

    if (!table->loaded || table->row_count == 0) {
        return SUCCESS;
    }

    matches = (int *)malloc((size_t)table->row_count * sizeof(int));
    if (matches == NULL) {
        utils_set_error(error, error_size, "Failed to allocate memory.");
        return FAILURE;
    }

    if (where == NULL) {
        for (i = 0; i < table->row_count; i++) {
            matches[i] = i;
        }
        *out_row_indices = matches;
        *out_count = table->row_count;
        return SUCCESS;
    }

    where_column_index = table_find_column_index(table, where->column);
    if (where_column_index == FAILURE) {
        utils_set_error(error, error_size, "Column '%s' not found.", where->column);
        free(matches);
        return FAILURE;
    }

    match_count = 0;
    for (i = 0; i < table->row_count; i++) {
        int comparison = utils_compare_values(table->rows[i][where_column_index],
                                              where->value);
        if (table_where_matches(comparison, where->op)) {
            matches[match_count++] = i;
        }
    }

    if (match_count == 0) {
        free(matches);
        return SUCCESS;
    }

    *out_row_indices = matches;
    *out_count = match_count;
    return SUCCESS;
}

int table_linear_scan_by_field(const TableRuntime *table,
                               const WhereClause *where,
                               int **out_row_indices, int *out_count) {
    return table_linear_scan_by_field_with_error(table, where, out_row_indices,
                                                 out_count, NULL, 0);
}

void table_runtime_cleanup(void) {
    TableRuntimeEntry *entry;
    TableRuntimeEntry *next;

    if (pthread_mutex_lock(&table_runtime_registry_lock) != 0) {
        return;
    }

    entry = table_runtime_registry_head;
    table_runtime_registry_head = NULL;
    pthread_mutex_unlock(&table_runtime_registry_lock);

    while (entry != NULL) {
        next = entry->next;
        table_runtime_destroy_entry(entry);
        entry = next;
    }
}
