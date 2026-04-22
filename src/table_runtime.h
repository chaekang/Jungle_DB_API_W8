#ifndef TABLE_RUNTIME_H
#define TABLE_RUNTIME_H

#include "parser.h"

struct BPTreeNode;
struct TableRuntimeEntry;

typedef struct {
    char table_name[MAX_IDENTIFIER_LEN];
    int col_count;
    char columns[MAX_COLUMNS][MAX_IDENTIFIER_LEN];
    ValueKind column_value_kinds[MAX_COLUMNS];
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
} TableRuntimeHandle;

void table_init(TableRuntime *table);
void table_free(TableRuntime *table);
int table_reserve_if_needed(TableRuntime *table);

/*
 * SELECT 경로는 read lock을 잡는다. 테이블이 없으면 새 엔트리를 만들지 않는다.
 */
int table_runtime_acquire_read(const char *table_name, TableRuntimeHandle *out_handle);

/*
 * INSERT 경로는 write lock을 잡는다. 테이블이 없으면 새 엔트리를 만든다.
 */
int table_runtime_acquire_write(const char *table_name, TableRuntimeHandle *out_handle);

/*
 * 기존 테스트/벤치마크 호환용 이름이다. 새 코드는 read/write API를 직접 쓴다.
 */
int table_runtime_acquire(const char *table_name, TableRuntimeHandle *out_handle);
TableRuntime *table_runtime_handle_table(TableRuntimeHandle *handle);
void table_runtime_release(TableRuntimeHandle *handle);

int table_insert_row(TableRuntime *table, const InsertStatement *stmt,
                     int *out_row_index);
char **table_get_row_by_slot(const TableRuntime *table, int row_index);
int table_linear_scan_by_field(const TableRuntime *table,
                               const WhereClause *where,
                               int **out_row_indices, int *out_count);
void table_runtime_cleanup(void);
const char *table_runtime_get_last_error(void);

/*
 * 테스트용 registry entry 개수 조회 함수다.
 */
int table_runtime_registry_count(void);

#endif
