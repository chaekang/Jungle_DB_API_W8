#include "benchmark.h"
#include "engine.h"
#include "query_result.h"
#include "server.h"
#include "table_runtime.h"
#include "tokenizer.h"
#include "utils.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t main_skip_whitespace(const char *text, size_t index) {
    while (text[index] != '\0' && isspace((unsigned char)text[index])) {
        index++;
    }
    return index;
}

static void main_print_border(const int *widths, int col_count) {
    int i;
    int j;

    for (i = 0; i < col_count; i++) {
        putchar('+');
        for (j = 0; j < widths[i] + 2; j++) {
            putchar('-');
        }
    }
    puts("+");
}

static void main_print_table_result(const QueryResult *result) {
    int widths[MAX_COLUMNS];
    int i;
    int j;
    int cell_width;

    for (i = 0; i < result->column_count; i++) {
        widths[i] = utils_display_width(result->columns[i]);
    }

    for (i = 0; i < result->row_count; i++) {
        for (j = 0; j < result->column_count; j++) {
            cell_width = utils_display_width(result->rows[i][j]);
            if (cell_width > widths[j]) {
                widths[j] = cell_width;
            }
        }
    }

    main_print_border(widths, result->column_count);
    for (i = 0; i < result->column_count; i++) {
        printf("| ");
        utils_print_padded(stdout, result->columns[i], widths[i]);
        putchar(' ');
    }
    puts("|");
    main_print_border(widths, result->column_count);

    for (i = 0; i < result->row_count; i++) {
        for (j = 0; j < result->column_count; j++) {
            printf("| ");
            utils_print_padded(stdout, result->rows[i][j], widths[j]);
            putchar(' ');
        }
        puts("|");
    }

    main_print_border(widths, result->column_count);
    printf("%d row%s selected.\n", result->row_count,
           result->row_count == 1 ? "" : "s");
}

static int main_print_result(const QueryResult *result) {
    if (result == NULL) {
        return FAILURE;
    }

    if (!result->success) {
        fprintf(stderr, "Error: %s\n",
                result->error[0] == '\0' ? "Unknown error." : result->error);
        return FAILURE;
    }

    if (result->kind == QUERY_RESULT_MESSAGE) {
        puts(result->message);
        return SUCCESS;
    }

    if (result->kind == QUERY_RESULT_TABLE) {
        main_print_table_result(result);
        return SUCCESS;
    }

    return SUCCESS;
}

static int main_process_sql_statement(const char *sql) {
    QueryResult result;
    int status;

    if (sql == NULL) {
        return FAILURE;
    }

    query_result_init(&result);
    status = engine_execute_sql(sql, &result);
    if (main_print_result(&result) != SUCCESS) {
        status = FAILURE;
    }
    query_result_free(&result);
    return status;
}

static int main_run_file_mode(const char *path) {
    char *content;
    size_t start;
    int terminator_index;
    char *statement;
    char *remaining;
    int overall_status;

    content = utils_read_file(path);
    if (content == NULL) {
        return FAILURE;
    }

    overall_status = SUCCESS;
    start = 0;
    while (content[start] != '\0') {
        start = main_skip_whitespace(content, start);
        if (content[start] == '\0') {
            break;
        }

        terminator_index = utils_find_statement_terminator(content, start);
        if (terminator_index == FAILURE) {
            remaining = utils_strdup(content + start);
            if (remaining == NULL) {
                free(content);
                return FAILURE;
            }
            utils_trim(remaining);
            if (remaining[0] != '\0') {
                fprintf(stderr, "Error: Missing semicolon at end of SQL statement.\n");
                overall_status = FAILURE;
            }
            free(remaining);
            break;
        }

        statement = utils_substring(content, start,
                                    (size_t)terminator_index - start + 1);
        if (statement == NULL) {
            free(content);
            return FAILURE;
        }

        if (main_process_sql_statement(statement) != SUCCESS) {
            overall_status = FAILURE;
        }
        free(statement);
        start = (size_t)terminator_index + 1;
    }

    free(content);
    return overall_status;
}

static int main_trimmed_equals(const char *line, const char *keyword) {
    char *copy;
    int result;

    copy = utils_strdup(line);
    if (copy == NULL) {
        return 0;
    }

    utils_trim(copy);
    result = utils_equals_ignore_case(copy, keyword);
    free(copy);
    return result;
}

static int main_replace_buffer_with_remainder(char **buffer, size_t *length,
                                              size_t *capacity, int end_index) {
    char *remainder;
    size_t remainder_length;

    if (buffer == NULL || *buffer == NULL || length == NULL || capacity == NULL) {
        return FAILURE;
    }

    remainder = utils_strdup(*buffer + end_index + 1);
    if (remainder == NULL) {
        return FAILURE;
    }

    utils_trim(remainder);
    free(*buffer);
    *buffer = remainder;
    remainder_length = strlen(remainder);
    *length = remainder_length;
    *capacity = remainder_length + 1;
    return SUCCESS;
}

static int main_run_repl_mode(void) {
    char line[MAX_SQL_LENGTH];
    char *buffer;
    size_t buffer_length;
    size_t buffer_capacity;
    int terminator_index;
    char *statement;
    int overall_status;

    buffer = NULL;
    buffer_length = 0;
    buffer_capacity = 0;
    overall_status = SUCCESS;

    while (1) {
        printf("%s", buffer_length == 0 ? "SQL> " : "...> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            if (buffer != NULL && buffer[0] != '\0') {
                fprintf(stderr, "Error: Incomplete SQL statement before EOF.\n");
                overall_status = FAILURE;
            }
            break;
        }

        if (buffer_length == 0 &&
            (main_trimmed_equals(line, "exit") || main_trimmed_equals(line, "quit"))) {
            break;
        }

        if (utils_append_buffer(&buffer, &buffer_length, &buffer_capacity, line) != SUCCESS) {
            free(buffer);
            return FAILURE;
        }

        while (buffer != NULL &&
               (terminator_index = utils_find_statement_terminator(buffer, 0)) != FAILURE) {
            statement = utils_substring(buffer, 0, (size_t)terminator_index + 1);
            if (statement == NULL) {
                free(buffer);
                return FAILURE;
            }

            if (main_process_sql_statement(statement) != SUCCESS) {
                overall_status = FAILURE;
            }
            free(statement);

            if (main_replace_buffer_with_remainder(&buffer, &buffer_length,
                                                   &buffer_capacity,
                                                   terminator_index) != SUCCESS) {
                free(buffer);
                return FAILURE;
            }

            if (buffer_length == 0) {
                free(buffer);
                buffer = NULL;
                buffer_capacity = 0;
                break;
            }
        }
    }

    free(buffer);
    puts("Bye.");
    return overall_status;
}

int main(int argc, char *argv[]) {
    int status;

    if (argc > 3) {
        fprintf(stderr,
                "Usage: %s [sql_file|--benchmark|benchmark|--server [port]]\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    if (argc >= 2 &&
        (utils_equals_ignore_case(argv[1], "--server") ||
         utils_equals_ignore_case(argv[1], "server"))) {
        int port = 8080;

        if (argc == 3) {
            if (!utils_is_integer(argv[2])) {
                fprintf(stderr, "Error: Server port must be an integer.\n");
                return EXIT_FAILURE;
            }
            port = (int)utils_parse_integer(argv[2]);
        }

        status = server_run_forever(port, 8, 32);
    } else if (argc == 2 &&
               (utils_equals_ignore_case(argv[1], "--benchmark") ||
                utils_equals_ignore_case(argv[1], "benchmark"))) {
        BenchmarkConfig config = benchmark_default_config();
        status = benchmark_run(&config);
    } else if (argc == 2) {
        status = main_run_file_mode(argv[1]);
    } else {
        status = main_run_repl_mode();
    }

    table_runtime_cleanup();
    tokenizer_cleanup_cache();
    return status == SUCCESS ? EXIT_SUCCESS : EXIT_FAILURE;
}
