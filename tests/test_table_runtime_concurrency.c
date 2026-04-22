#include "table_runtime.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define INSERT_COUNT 1000
#define HOLD_USEC 200000

typedef struct {
    const char *table_name;
    const char *name_prefix;
    int start_age;
} InsertThreadArgs;

typedef struct {
    const char *table_name;
    volatile int acquired;
    volatile int released;
} ReadHoldArgs;

typedef struct {
    const char *table_name;
    volatile int finished;
    volatile int succeeded;
} WriteAttemptArgs;

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
        handle.lock_mode = TABLE_RUNTIME_LOCK_NONE;
        snprintf(name, sizeof(name), "%s_%d", args->name_prefix, i);
        snprintf(age, sizeof(age), "%d", args->start_age + (i % 50));
        prepare_insert(&stmt, args->table_name, name, age);

        if (table_runtime_acquire_write(args->table_name, &handle) != SUCCESS) {
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

static void *read_hold_worker(void *arg) {
    ReadHoldArgs *args;
    TableRuntimeHandle handle;

    args = (ReadHoldArgs *)arg;
    handle.entry = NULL;
    handle.lock_mode = TABLE_RUNTIME_LOCK_NONE;
    if (table_runtime_acquire_read(args->table_name, &handle) != SUCCESS) {
        return (void *)1;
    }

    args->acquired = 1;
    usleep(HOLD_USEC);
    table_runtime_release(&handle);
    args->released = 1;
    return NULL;
}

static void *write_attempt_worker(void *arg) {
    WriteAttemptArgs *args;
    TableRuntimeHandle handle;
    TableRuntime *table;
    InsertStatement stmt;
    int row_index;

    args = (WriteAttemptArgs *)arg;
    handle.entry = NULL;
    handle.lock_mode = TABLE_RUNTIME_LOCK_NONE;
    prepare_insert(&stmt, args->table_name, "writer", "99");

    if (table_runtime_acquire_write(args->table_name, &handle) == SUCCESS) {
        table = table_runtime_handle_table(&handle);
        if (table != NULL && table_insert_row(table, &stmt, &row_index) == SUCCESS) {
            args->succeeded = 1;
        }
        table_runtime_release(&handle);
    }

    args->finished = 1;
    return NULL;
}

int main(void) {
    pthread_t thread_a;
    pthread_t thread_b;
    pthread_t read_thread;
    pthread_t write_thread;
    InsertThreadArgs args_a;
    InsertThreadArgs args_b;
    ReadHoldArgs read_args;
    WriteAttemptArgs write_args;
    void *thread_result;
    TableRuntimeHandle handle;
    TableRuntime *table;
    InsertStatement seed_stmt;
    int row_index;
    int registry_before;

    table_runtime_cleanup();

    handle.entry = NULL;
    handle.lock_mode = TABLE_RUNTIME_LOCK_NONE;
    if (assert_true(table_runtime_acquire_write("shared_table", &handle) == SUCCESS,
                    "shared_table should be creatable for concurrency tests") != SUCCESS) {
        return EXIT_FAILURE;
    }
    prepare_insert(&seed_stmt, "shared_table", "seed", "30");
    if (assert_true(table_insert_row(table_runtime_handle_table(&handle), &seed_stmt, &row_index) ==
                        SUCCESS,
                    "shared_table seed insert should succeed") != SUCCESS) {
        table_runtime_release(&handle);
        return EXIT_FAILURE;
    }
    table_runtime_release(&handle);

    memset(&read_args, 0, sizeof(read_args));
    read_args.table_name = "shared_table";
    if (assert_true(pthread_create(&read_thread, NULL, read_hold_worker, &read_args) == 0,
                    "read hold thread should start") != SUCCESS) {
        return EXIT_FAILURE;
    }

    while (!read_args.acquired) {
        usleep(1000);
    }

    handle.entry = NULL;
    handle.lock_mode = TABLE_RUNTIME_LOCK_NONE;
    if (assert_true(table_runtime_acquire_read("shared_table", &handle) == SUCCESS,
                    "second read acquire should succeed while another read is active") != SUCCESS) {
        return EXIT_FAILURE;
    }
    table_runtime_release(&handle);

    memset(&write_args, 0, sizeof(write_args));
    write_args.table_name = "shared_table";
    if (assert_true(pthread_create(&write_thread, NULL, write_attempt_worker, &write_args) == 0,
                    "write thread should start") != SUCCESS) {
        return EXIT_FAILURE;
    }

    usleep(50000);
    if (assert_true(write_args.finished == 0,
                    "write acquire should wait while read lock is held") != SUCCESS) {
        return EXIT_FAILURE;
    }

    if (assert_true(pthread_join(read_thread, &thread_result) == 0,
                    "read thread should join") != SUCCESS ||
        assert_true(thread_result == NULL, "read thread should succeed") != SUCCESS ||
        assert_true(pthread_join(write_thread, &thread_result) == 0,
                    "write thread should join") != SUCCESS ||
        assert_true(thread_result == NULL, "write thread should succeed") != SUCCESS ||
        assert_true(write_args.succeeded == 1,
                    "write thread should eventually succeed after readers release") != SUCCESS) {
        return EXIT_FAILURE;
    }

    args_a.table_name = "same_table";
    args_a.name_prefix = "alpha";
    args_a.start_age = 20;
    args_b.table_name = "same_table";
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

    handle.entry = NULL;
    handle.lock_mode = TABLE_RUNTIME_LOCK_NONE;
    if (assert_true(table_runtime_acquire_read("same_table", &handle) == SUCCESS,
                    "same_table should be readable after concurrent inserts") != SUCCESS) {
        return EXIT_FAILURE;
    }

    table = table_runtime_handle_table(&handle);
    if (assert_true(table != NULL, "same_table handle should expose runtime") != SUCCESS ||
        assert_true(table->row_count == INSERT_COUNT * 2,
                    "same_table should retain all concurrent inserts") != SUCCESS ||
        assert_true(table->next_id == (INSERT_COUNT * 2) + 1,
                    "same_table ids should advance without duplication") != SUCCESS) {
        table_runtime_release(&handle);
        return EXIT_FAILURE;
    }
    table_runtime_release(&handle);

    registry_before = table_runtime_registry_entry_count();
    if (assert_true(table_runtime_acquire_read("ghost_table", &handle) == TABLE_RUNTIME_NOT_FOUND,
                    "missing table read flood should keep failing cleanly") != SUCCESS ||
        assert_true(table_runtime_acquire_read("ghost_table", &handle) == TABLE_RUNTIME_NOT_FOUND,
                    "missing table read flood should remain stable") != SUCCESS ||
        assert_true(table_runtime_registry_entry_count() == registry_before,
                    "missing table reads should not create registry entries") != SUCCESS) {
        return EXIT_FAILURE;
    }

    table_runtime_cleanup();
    puts("[PASS] table_runtime_concurrency");
    return EXIT_SUCCESS;
}
