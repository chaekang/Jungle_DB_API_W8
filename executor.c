#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mini_db.h"

#define TABLE_DEFAULT_CELL_WIDTH 24
#define TABLE_ID_CELL_WIDTH 8

/* 내부 구현: 실행 흐름을 구성하는 private 함수 목록이다. */
typedef struct {
    const TableMetadata *table;
    int has_error;
    const struct TablePrinter *printer;
} BetweenSelectContext;

typedef struct TablePrinter {
    const TableMetadata *table;
    int widths[MAX_VALUES];
} TablePrinter;

static void execute_select(const Plan *plan);
static void execute_select_all_fixed_rows(const TableMetadata *table);
static void execute_select_by_id(const Plan *plan, const TableMetadata *table);
static void execute_select_by_id_between(const Plan *plan, const TableMetadata *table);
static void execute_insert(const Plan *plan);
static void init_table_printer(TablePrinter *printer, const TableMetadata *table);
static void print_table_header(const TablePrinter *printer);
static void print_table_footer(const TablePrinter *printer);
static void print_table_border(const TablePrinter *printer);
static void print_table_row(const TablePrinter *printer, const char *logical_row);
static void print_table_cell(const char *value, int width);
static int split_logical_row_value(const char **cursor, char *value, size_t value_size);
static int column_width(const char *column_name);
static void print_repeated_char(char value, int count);
static int encode_fixed_row(const Plan *plan, char fixed_row[ROW_SIZE]);
static int decode_fixed_row(const char fixed_row[ROW_SIZE], char *logical_row, size_t logical_row_size);
static int parse_id_value(const char *text, int *id);
static int read_fixed_row_at(const TableMetadata *table, RowLocation location, char fixed_row[ROW_SIZE]);
static int print_between_row(int id, RowLocation location, void *context);

/* 4.1 실행 분기: 파싱된 계획을 SELECT 또는 INSERT 실행으로 보낸다. */
void execute_plan(const Plan *plan) {
    if (plan->type == QUERY_SELECT) {
        execute_select(plan);
        return;
    }

    if (plan->type == QUERY_INSERT) {
        execute_insert(plan);
        return;
    }

    printf("실행할 수 없는 계획입니다\n");
}

/* 4.1 실행 분기: SELECT 조건 유무에 따라 전체 조회와 id 인덱스 조회로 나눈다. */
static void execute_select(const Plan *plan) {
    const TableMetadata *table = find_table(plan->table_name);

    if (table == NULL) {
        printf("실행할 수 없는 계획입니다\n");
        return;
    }

    if (plan->condition.type == SELECT_CONDITION_NONE) {
        execute_select_all_fixed_rows(table);
        return;
    }

    if (plan->condition.type == SELECT_CONDITION_ID_EQUALS) {
        execute_select_by_id(plan, table);
        return;
    }

    if (plan->condition.type == SELECT_CONDITION_ID_BETWEEN) {
        execute_select_by_id_between(plan, table);
        return;
    }

    printf("지원하지 않는 조회 조건입니다\n");
}

/* 4.2 SELECT 전체 조회: 데이터 파일을 고정 길이 row 단위로 읽어 출력한다. */
static void execute_select_all_fixed_rows(const TableMetadata *table) {
    FILE *file;
    char fixed_row[ROW_SIZE];
    char logical_row[MAX_INPUT_SIZE];
    int has_error = 0;
    TablePrinter printer;

    file = fopen(table->csv_file_path, "rb");
    if (file == NULL) {
        printf("CSV 파일을 열 수 없습니다\n");
        return;
    }

    init_table_printer(&printer, table);
    print_table_header(&printer);
    while (1) {
        size_t read_size = fread(fixed_row, 1, ROW_SIZE, file);

        if (read_size == 0) {
            break;
        }

        if (read_size != ROW_SIZE || !decode_fixed_row(fixed_row, logical_row, sizeof(logical_row))) {
            printf("데이터 파일이 올바르지 않습니다\n");
            has_error = 1;
            break;
        }

        print_table_row(&printer, logical_row);
    }

    if (!has_error) {
        print_table_footer(&printer);
    }
    fclose(file);
}

/* 4.3 SELECT id 조건 조회: B+Tree 인덱스에서 위치를 찾고 해당 row만 읽는다. */
static void execute_select_by_id(const Plan *plan, const TableMetadata *table) {
    RowLocation location;
    char fixed_row[ROW_SIZE];
    char logical_row[MAX_INPUT_SIZE];
    int found;
    TablePrinter printer;

    found = db_index_get(plan->table_name, plan->condition.id_value, &location);
    if (found == 0) {
        init_table_printer(&printer, table);
        print_table_header(&printer);
        print_table_footer(&printer);
        return;
    }
    if (found < 0) {
        printf("인덱스를 조회할 수 없습니다\n");
        return;
    }

    if (!read_fixed_row_at(table, location, fixed_row) ||
        !decode_fixed_row(fixed_row, logical_row, sizeof(logical_row))) {
        printf("데이터 파일이 올바르지 않습니다\n");
        return;
    }

    init_table_printer(&printer, table);
    print_table_header(&printer);
    print_table_row(&printer, logical_row);
    print_table_footer(&printer);
}

/* 4.3 SELECT id 범위 조회: B+Tree 인덱스에서 BETWEEN 범위의 row 위치를 순서대로 읽는다. */
static void execute_select_by_id_between(const Plan *plan, const TableMetadata *table) {
    TablePrinter printer;
    init_table_printer(&printer, table);
    BetweenSelectContext context = {table, 0, &printer};
    int result;

    print_table_header(&printer);
    result = db_index_get_between(plan->table_name, plan->condition.id_start_value, plan->condition.id_end_value,
                                  print_between_row, &context);
    if (context.has_error) {
        printf("데이터 파일이 올바르지 않습니다\n");
        return;
    }
    if (result < 0) {
        printf("인덱스를 조회할 수 없습니다\n");
        return;
    }

    print_table_footer(&printer);
}

/* 4.4 INSERT 행 추가: 고정 길이 row를 파일 끝에 쓰고 인덱스를 갱신한다. */
static void execute_insert(const Plan *plan) {
    const TableMetadata *table = find_table(plan->table_name);
    FILE *file;
    RowLocation location;
    char fixed_row[ROW_SIZE];
    int id;
    int found;

    if (table == NULL) {
        printf("실행할 수 없는 계획입니다\n");
        return;
    }

    if (!parse_id_value(plan->values[0], &id)) {
        printf("id는 정수여야 합니다\n");
        return;
    }

    found = db_index_get(plan->table_name, id, &location);
    if (found > 0) {
        printf("이미 존재하는 id입니다\n");
        return;
    }
    if (found < 0) {
        printf("인덱스를 조회할 수 없습니다\n");
        return;
    }

    if (!encode_fixed_row(plan, fixed_row)) {
        printf("row size를 초과했습니다\n");
        return;
    }

    file = fopen(table->csv_file_path, "ab+");
    if (file == NULL) {
        printf("CSV 파일을 열 수 없습니다\n");
        return;
    }

    fseek(file, 0, SEEK_END);
    location.offset = ftell(file);
    if (location.offset < 0 || location.offset % table->row_size != 0 ||
        fwrite(fixed_row, 1, ROW_SIZE, file) != ROW_SIZE || fflush(file) != 0) {
        printf("데이터를 저장할 수 없습니다\n");
        fclose(file);
        return;
    }

    fclose(file);

    if (db_index_put(plan->table_name, id, location) != 1) {
        printf("인덱스를 갱신할 수 없습니다\n");
    }
}

/* 내부 구현: 컬럼명 기준으로 표 출력 상태를 초기화한다. */
static void init_table_printer(TablePrinter *printer, const TableMetadata *table) {
    printer->table = table;
    for (int i = 0; i < table->column_count; i++) {
        printer->widths[i] = column_width(table->columns[i]);
    }
}

/* 내부 구현: SELECT 결과의 컬럼명을 표 형태로 출력한다. */
static void print_table_header(const TablePrinter *printer) {
    print_table_border(printer);
    for (int i = 0; i < printer->table->column_count; i++) {
        print_table_cell(printer->table->columns[i], printer->widths[i]);
    }
    printf("|\n");
    print_table_border(printer);
}

/* 내부 구현: SELECT 결과 표의 마지막 경계선을 출력한다. */
static void print_table_footer(const TablePrinter *printer) {
    print_table_border(printer);
}

/* 내부 구현: 컬럼 개수에 맞춰 표의 가로 경계선을 출력한다. */
static void print_table_border(const TablePrinter *printer) {
    for (int i = 0; i < printer->table->column_count; i++) {
        printf("+");
        print_repeated_char('-', printer->widths[i] + 2);
    }
    printf("+\n");
}

/* 내부 구현: CSV 형태의 논리 row를 표의 한 행으로 출력한다. */
static void print_table_row(const TablePrinter *printer, const char *logical_row) {
    const char *cursor = logical_row;

    for (int i = 0; i < printer->table->column_count; i++) {
        char value[MAX_VALUE_SIZE];

        split_logical_row_value(&cursor, value, sizeof(value));
        print_table_cell(value, printer->widths[i]);
    }
    printf("|\n");
}

/* 내부 구현: 한 칸의 내용을 고정 폭에 맞춰 출력한다. 너무 긴 값은 ...로 줄인다. */
static void print_table_cell(const char *value, int width) {
    char cell[TABLE_DEFAULT_CELL_WIDTH + 1];
    size_t length = strlen(value);

    if ((int) length > width) {
        size_t copy_length = width > 3 ? (size_t) width - 3 : (size_t) width;

        memcpy(cell, value, copy_length);
        if (width > 3) {
            memcpy(cell + copy_length, "...", 3);
        }
        cell[width] = '\0';
    } else {
        snprintf(cell, sizeof(cell), "%s", value);
    }

    printf("| %-*s ", width, cell);
}

/* 내부 구현: CSV row에서 다음 컬럼 값을 하나 꺼낸다. */
static int split_logical_row_value(const char **cursor, char *value, size_t value_size) {
    const char *comma = strchr(*cursor, ',');
    size_t length = comma != NULL ? (size_t) (comma - *cursor) : strlen(*cursor);

    if (length >= value_size) {
        length = value_size - 1;
    }

    memcpy(value, *cursor, length);
    value[length] = '\0';

    if (comma != NULL) {
        *cursor = comma + 1;
    } else {
        *cursor += strlen(*cursor);
    }

    return 1;
}

/* 내부 구현: 컬럼별 기본 출력 폭을 정한다. */
static int column_width(const char *column_name) {
    if (strcmp(column_name, "id") == 0) {
        return TABLE_ID_CELL_WIDTH;
    }
    return TABLE_DEFAULT_CELL_WIDTH;
}

/* 내부 구현: 표 경계선을 만들 때 같은 문자를 반복 출력한다. */
static void print_repeated_char(char value, int count) {
    for (int i = 0; i < count; i++) {
        putchar(value);
    }
}

/* 5.2 고정 길이 row 저장: INSERT 값 목록을 64 bytes fixed row로 변환한다. */
static int encode_fixed_row(const Plan *plan, char fixed_row[ROW_SIZE]) {
    size_t length = 0;

    memset(fixed_row, ROW_PADDING_CHAR, ROW_SIZE);
    for (int i = 0; i < plan->value_count; i++) {
        size_t value_length = strlen(plan->values[i]);

        if (length + value_length + 1 > ROW_DATA_SIZE) {
            return 0;
        }

        memcpy(fixed_row + length, plan->values[i], value_length);
        length += value_length;
        fixed_row[length] = ',';
        length++;
    }

    fixed_row[ROW_DATA_SIZE] = '\n';
    return 1;
}

/* 5.3 논리 row 변환: fixed row의 padding을 제거하고 출력용 row로 바꾼다. */
static int decode_fixed_row(const char fixed_row[ROW_SIZE], char *logical_row, size_t logical_row_size) {
    int end = ROW_DATA_SIZE - 1;

    if (logical_row_size < ROW_SIZE || fixed_row[ROW_DATA_SIZE] != '\n') {
        return 0;
    }

    memcpy(logical_row, fixed_row, ROW_DATA_SIZE);
    logical_row[ROW_DATA_SIZE] = '\0';

    while (end >= 0 && logical_row[end] == ROW_PADDING_CHAR) {
        logical_row[end] = '\0';
        end--;
    }

    if (end >= 0 && logical_row[end] == ',') {
        logical_row[end] = '\0';
    }

    return 1;
}

/* 내부 구현: B+Tree key로 사용할 첫 번째 컬럼 id를 정수로 해석한다. */
static int parse_id_value(const char *text, int *id) {
    char *end;
    long parsed;

    if (text[0] == '\0') {
        return 0;
    }

    parsed = strtol(text, &end, 10);
    if (*end != '\0' || parsed < INT_MIN || parsed > INT_MAX) {
        return 0;
    }

    *id = (int) parsed;
    return 1;
}

/* 5.4 위치 기반 읽기/쓰기: 인덱스가 알려준 byte offset에서 fixed row 하나를 읽는다. */
static int read_fixed_row_at(const TableMetadata *table, RowLocation location, char fixed_row[ROW_SIZE]) {
    FILE *file;
    size_t read_size;

    if (location.offset < 0 || location.offset % table->row_size != 0) {
        return 0;
    }

    file = fopen(table->csv_file_path, "rb");
    if (file == NULL) {
        return 0;
    }

    if (fseek(file, location.offset, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }

    read_size = fread(fixed_row, 1, ROW_SIZE, file);
    fclose(file);
    return read_size == ROW_SIZE;
}

static int print_between_row(int id, RowLocation location, void *context) {
    BetweenSelectContext *select_context = (BetweenSelectContext *) context;
    char fixed_row[ROW_SIZE];
    char logical_row[MAX_INPUT_SIZE];

    (void) id;
    if (!read_fixed_row_at(select_context->table, location, fixed_row) ||
        !decode_fixed_row(fixed_row, logical_row, sizeof(logical_row))) {
        select_context->has_error = 1;
        return -1;
    }

    print_table_row(select_context->printer, logical_row);
    return 0;
}
