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
    pthread_mutex_t lock;
    struct TableRuntimeEntry *next;
} TableRuntimeEntry;

static TableRuntimeEntry *table_runtime_registry_head = NULL;
static pthread_mutex_t table_runtime_registry_lock = PTHREAD_MUTEX_INITIALIZER;

static void table_free_owned_row(char **row, int col_count);
static int table_find_column_index(const TableRuntime *table, const char *target);
static int table_validate_insert_schema(const TableRuntime *table,
                                        const InsertStatement *stmt);
static int table_initialize_schema(TableRuntime *table,
                                   const InsertStatement *stmt);
static char **table_build_row(const TableRuntime *table, const InsertStatement *stmt,
                              long long next_id);
static int table_where_matches(int comparison, const char *op);
static TableRuntimeEntry *table_runtime_find_entry_locked(const char *table_name);
static TableRuntimeEntry *table_runtime_create_entry(const char *table_name);
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
                                        const InsertStatement *stmt) {
    int i;

    if (table == NULL || stmt == NULL) {
        return FAILURE;
    }

    if (stmt->column_count <= 0 || stmt->column_count != table->col_count - 1) {
        fprintf(stderr, "Error: INSERT columns do not match table schema.\n");
        return FAILURE;
    }

    for (i = 0; i < stmt->column_count; i++) {
        if (!utils_equals_ignore_case(table->columns[i + 1], stmt->columns[i])) {
            fprintf(stderr, "Error: INSERT columns do not match table schema.\n");
            return FAILURE;
        }
    }

    return SUCCESS;
}

static int table_initialize_schema(TableRuntime *table,
                                   const InsertStatement *stmt) {
    int i;

    if (table == NULL || stmt == NULL) {
        return FAILURE;
    }

    if (stmt->column_count <= 0 || stmt->column_count + 1 > MAX_COLUMNS) {
        fprintf(stderr, "Error: Invalid INSERT column count.\n");
        return FAILURE;
    }

    table->col_count = stmt->column_count + 1;
    if (utils_safe_strcpy(table->columns[0], sizeof(table->columns[0]), "id") != SUCCESS) {
        return FAILURE;
    }

    for (i = 0; i < stmt->column_count; i++) {
        if (utils_equals_ignore_case(stmt->columns[i], "id")) {
            fprintf(stderr, "Error: Explicit id values are not allowed.\n");
            return FAILURE;
        }

        if (utils_safe_strcpy(table->columns[i + 1], sizeof(table->columns[i + 1]),
                              stmt->columns[i]) != SUCCESS) {
            fprintf(stderr, "Error: Column name is too long.\n");
            return FAILURE;
        }
    }

    table->id_column_index = 0;
    table->loaded = 1;
    return SUCCESS;
}

static char **table_build_row(const TableRuntime *table, const InsertStatement *stmt,
                              long long next_id) {
    char **row;
    char id_buffer[MAX_VALUE_LEN];
    int i;

    if (table == NULL || stmt == NULL) {
        return NULL;
    }

    row = (char **)calloc((size_t)table->col_count, sizeof(char *));
    if (row == NULL) {
        fprintf(stderr, "Error: Failed to allocate memory.\n");
        return NULL;
    }

    snprintf(id_buffer, sizeof(id_buffer), "%lld", next_id);
    row[0] = utils_strdup(id_buffer);
    if (row[0] == NULL) {
        free(row);
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

static TableRuntimeEntry *table_runtime_create_entry(const char *table_name) {
    TableRuntimeEntry *entry;

    entry = (TableRuntimeEntry *)calloc(1, sizeof(*entry));
    if (entry == NULL) {
        fprintf(stderr, "Error: Failed to allocate memory.\n");
        return NULL;
    }

    table_init(&entry->table);
    if (utils_safe_strcpy(entry->table.table_name, sizeof(entry->table.table_name),
                          table_name) != SUCCESS) {
        fprintf(stderr, "Error: Table name is too long.\n");
        free(entry);
        return NULL;
    }

    if (pthread_mutex_init(&entry->lock, NULL) != 0) {
        fprintf(stderr, "Error: Failed to initialize table runtime lock.\n");
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
    pthread_mutex_destroy(&entry->lock);
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
        fprintf(stderr, "Error: Failed to allocate memory.\n");
        return FAILURE;
    }

    table->rows = new_rows;
    table->capacity = new_capacity;
    return SUCCESS;
}

int table_runtime_acquire(const char *table_name, TableRuntimeHandle *out_handle) {
    TableRuntimeEntry *entry;

    if (table_name == NULL || out_handle == NULL) {
        return FAILURE;
    }

    out_handle->entry = NULL;
    if (table_name[0] == '\0') {
        fprintf(stderr, "Error: Table name is empty.\n");
        return FAILURE;
    }

    if (pthread_mutex_lock(&table_runtime_registry_lock) != 0) {
        fprintf(stderr, "Error: Failed to lock table runtime registry.\n");
        return FAILURE;
    }

    entry = table_runtime_find_entry_locked(table_name);
    if (entry == NULL) {
        entry = table_runtime_create_entry(table_name);
    }

    pthread_mutex_unlock(&table_runtime_registry_lock);

    if (entry == NULL) {
        return FAILURE;
    }

    if (pthread_mutex_lock(&entry->lock) != 0) {
        fprintf(stderr, "Error: Failed to lock table runtime.\n");
        return FAILURE;
    }

    out_handle->entry = entry;
    return SUCCESS;
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

    pthread_mutex_unlock(&handle->entry->lock);
    handle->entry = NULL;
}

int table_insert_row(TableRuntime *table, const InsertStatement *stmt,
                     int *out_row_index) {
    char **row;
    int row_index;

    if (table == NULL || stmt == NULL || out_row_index == NULL) {
        return FAILURE;
    }

    if (table->table_name[0] == '\0') {
        if (utils_safe_strcpy(table->table_name, sizeof(table->table_name),
                              stmt->table_name) != SUCCESS) {
            fprintf(stderr, "Error: Table name is too long.\n");
            return FAILURE;
        }
    }

    if (!utils_equals_ignore_case(table->table_name, stmt->table_name)) {
        fprintf(stderr, "Error: Active runtime table does not match INSERT target.\n");
        return FAILURE;
    }

    if (!table->loaded) {
        if (table_initialize_schema(table, stmt) != SUCCESS) {
            return FAILURE;
        }
    } else if (table_validate_insert_schema(table, stmt) != SUCCESS) {
        return FAILURE;
    }

    if (table_reserve_if_needed(table) != SUCCESS) {
        return FAILURE;
    }

    row = table_build_row(table, stmt, table->next_id);
    if (row == NULL) {
        return FAILURE;
    }

    if (table->next_id > INT_MAX) {
        fprintf(stderr, "Error: Runtime id exceeds B+ tree key range.\n");
        table_free_owned_row(row, table->col_count);
        return FAILURE;
    }

    row_index = table->row_count;
    table->rows[row_index] = row;
    if (bptree_insert(&table->id_index_root, (int)table->next_id, row_index) != SUCCESS) {
        table_free_owned_row(table->rows[row_index], table->col_count);
        table->rows[row_index] = NULL;
        return FAILURE;
    }

    *out_row_index = row_index;
    table->row_count++;
    table->next_id++;
    return SUCCESS;
}

char **table_get_row_by_slot(const TableRuntime *table, int row_index) {
    if (table == NULL || row_index < 0 || row_index >= table->row_count) {
        return NULL;
    }

    return table->rows[row_index];
}

int table_linear_scan_by_field(const TableRuntime *table,
                               const WhereClause *where,
                               int **out_row_indices, int *out_count) {
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
        fprintf(stderr, "Error: Failed to allocate memory.\n");
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
        fprintf(stderr, "Error: Column '%s' not found.\n", where->column);
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

void table_runtime_cleanup(void) {
    TableRuntimeEntry *entry;
    TableRuntimeEntry *next;

    if (pthread_mutex_lock(&table_runtime_registry_lock) != 0) {
        fprintf(stderr, "Error: Failed to lock table runtime registry.\n");
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
