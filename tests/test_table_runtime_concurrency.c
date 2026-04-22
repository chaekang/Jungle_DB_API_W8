#include "table_runtime.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INSERT_COUNT 1000

typedef struct {
    const char *table_name;
    const char *name_prefix;
    int start_age;
} InsertThreadArgs;

static int assert_true(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "[FAIL] %s\n", message);
        return FAILURE;
    }
    return SUCCESS;
}

static void prepare_insert(InsertStatement *stmt, const char *table_name,
                           const char *name, const char *age) {
    memset(stmt, 0, sizeof(*stmt));
    snprintf(stmt->table_name, sizeof(stmt->table_name), "%s", table_name);
    stmt->column_count = 2;
    snprintf(stmt->columns[0], sizeof(stmt->columns[0]), "name");
    snprintf(stmt->columns[1], sizeof(stmt->columns[1]), "age");
    snprintf(stmt->values[0], sizeof(stmt->values[0]), "%s", name);
    snprintf(stmt->values[1], sizeof(stmt->values[1]), "%s", age);
    stmt->value_kinds[0] = VALUE_KIND_STRING;
    stmt->value_kinds[1] = VALUE_KIND_INT;
}

static void *insert_rows_worker(void *arg) {
    InsertThreadArgs *args;
    int i;

    args = (InsertThreadArgs *)arg;
    for (i = 0; i < INSERT_COUNT; i++) {
        TableRuntimeHandle handle;
        TableRuntime *table;
        InsertStatement stmt;
        char name[MAX_VALUE_LEN];
        char age[MAX_VALUE_LEN];
        int row_index;

        handle.entry = NULL;
        snprintf(name, sizeof(name), "%s_%d", args->name_prefix, i);
        snprintf(age, sizeof(age), "%d", args->start_age + (i % 50));
        prepare_insert(&stmt, args->table_name, name, age);

        if (table_runtime_acquire(args->table_name, &handle) != SUCCESS) {
            return (void *)1;
        }

        table = table_runtime_handle_table(&handle);
        if (table == NULL || table_insert_row(table, &stmt, &row_index) != SUCCESS) {
            table_runtime_release(&handle);
            return (void *)1;
        }

        table_runtime_release(&handle);
    }

    return NULL;
}

int main(void) {
    pthread_t thread_a;
    pthread_t thread_b;
    InsertThreadArgs args_a;
    InsertThreadArgs args_b;
    void *thread_result;
    TableRuntimeHandle handle_a;
    TableRuntimeHandle handle_b;
    TableRuntime *table_a;
    TableRuntime *table_b;

    table_runtime_cleanup();

    args_a.table_name = "table_a";
    args_a.name_prefix = "alpha";
    args_a.start_age = 20;
    args_b.table_name = "table_b";
    args_b.name_prefix = "beta";
    args_b.start_age = 40;

    if (assert_true(pthread_create(&thread_a, NULL, insert_rows_worker, &args_a) == 0,
                    "thread A should start") != SUCCESS ||
        assert_true(pthread_create(&thread_b, NULL, insert_rows_worker, &args_b) == 0,
                    "thread B should start") != SUCCESS) {
        return EXIT_FAILURE;
    }

    if (assert_true(pthread_join(thread_a, &thread_result) == 0,
                    "thread A should join") != SUCCESS ||
        assert_true(thread_result == NULL, "thread A should finish successfully") != SUCCESS ||
        assert_true(pthread_join(thread_b, &thread_result) == 0,
                    "thread B should join") != SUCCESS ||
        assert_true(thread_result == NULL, "thread B should finish successfully") != SUCCESS) {
        return EXIT_FAILURE;
    }

    handle_a.entry = NULL;
    handle_b.entry = NULL;

    if (assert_true(table_runtime_acquire("table_a", &handle_a) == SUCCESS,
                    "table_a should be acquireable after threaded inserts") != SUCCESS) {
        return EXIT_FAILURE;
    }
    if (assert_true(table_runtime_acquire("table_b", &handle_b) == SUCCESS,
                    "table_b should be acquireable after threaded inserts") != SUCCESS) {
        table_runtime_release(&handle_a);
        return EXIT_FAILURE;
    }

    table_a = table_runtime_handle_table(&handle_a);
    table_b = table_runtime_handle_table(&handle_b);
    if (assert_true(table_a != NULL && table_b != NULL,
                    "handles should expose both tables") != SUCCESS ||
        assert_true(table_a->row_count == INSERT_COUNT,
                    "table_a should retain all inserts") != SUCCESS ||
        assert_true(table_b->row_count == INSERT_COUNT,
                    "table_b should retain all inserts") != SUCCESS ||
        assert_true(table_a->next_id == INSERT_COUNT + 1,
                    "table_a next_id should remain isolated") != SUCCESS ||
        assert_true(table_b->next_id == INSERT_COUNT + 1,
                    "table_b next_id should remain isolated") != SUCCESS) {
        table_runtime_release(&handle_b);
        table_runtime_release(&handle_a);
        return EXIT_FAILURE;
    }

    table_runtime_release(&handle_b);
    table_runtime_release(&handle_a);
    table_runtime_cleanup();
    puts("[PASS] table_runtime_concurrency");
    return EXIT_SUCCESS;
}
