#ifndef TABLE_RUNTIME_H
#define TABLE_RUNTIME_H

#include "parser.h"

struct BPTreeNode;
struct TableRuntimeEntry;

typedef enum {
    TABLE_RUNTIME_LOCK_NONE,
    TABLE_RUNTIME_LOCK_READ,
    TABLE_RUNTIME_LOCK_WRITE
} TableRuntimeLockMode;

typedef struct {
    char table_name[MAX_IDENTIFIER_LEN];
    int col_count;
    char columns[MAX_COLUMNS][MAX_IDENTIFIER_LEN];
    char ***rows;
    int row_count;
    int capacity;
    int id_column_index;
    long long next_id;
    struct BPTreeNode *id_index_root;
    int loaded;
} TableRuntime;

typedef struct {
    struct TableRuntimeEntry *entry;
    TableRuntimeLockMode lock_mode;
} TableRuntimeHandle;

void table_init(TableRuntime *table);
void table_free(TableRuntime *table);
int table_reserve_if_needed(TableRuntime *table);

int table_runtime_acquire(const char *table_name, TableRuntimeHandle *out_handle);
int table_runtime_acquire_read(const char *table_name, TableRuntimeHandle *out_handle);
int table_runtime_acquire_write(const char *table_name, TableRuntimeHandle *out_handle);
TableRuntime *table_runtime_handle_table(TableRuntimeHandle *handle);
void table_runtime_release(TableRuntimeHandle *handle);

int table_insert_row(TableRuntime *table, const InsertStatement *stmt,
                     int *out_row_index);
int table_delete_where(TableRuntime *table, const DeleteStatement *stmt,
                       int *out_deleted_count);
char **table_get_row_by_slot(const TableRuntime *table, int row_index);
int table_linear_scan_by_field(const TableRuntime *table,
                               const WhereClause *where,
                               int **out_row_indices, int *out_count);
void table_runtime_cleanup(void);

#endif
