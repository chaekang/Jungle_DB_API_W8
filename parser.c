#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mini_db.h"

typedef struct {
    const char *cursor;
    int has_error;
    Plan error_plan;
} Parser;

/* 내부 구현: SQL 파싱 흐름을 구성하는 private 함수 목록이다. */
static Plan parse_select(const char *sql);
static Plan parse_insert(const char *sql);
static Parser make_parser(const char *sql);
static int starts_with(const char *text, const char *prefix);
static int chars_equal_ignore_case(char left, char right);
static char to_lower_ascii(char value);
static int ends_with_semicolon(const char *text);
static void skip_spaces(Parser *parser);
static void expect_text(Parser *parser, const char *expected);
static void expect_char(Parser *parser, char expected);
static void expect_end(Parser *parser);
static void read_name(Parser *parser, char *buffer, size_t buffer_size);
static void read_integer(Parser *parser, int *value);
static void read_until_semicolon(Parser *parser, char *buffer, size_t buffer_size);
static Plan build_select_plan(Parser *parser, const char *table_name, SelectCondition condition);
static Plan build_insert_plan(Parser *parser, const char *table_name, char *values_text);
static void set_error(Plan *plan, const char *message);
static void set_parser_error(Parser *parser, const char *message);
static int is_ascii_text(const char *text);
static char *trim(char *text);

/* 3.1 SQL 타입 판별: SELECT와 INSERT 중 어떤 문장인지 구분한다. */
Plan parse_sql(const char *sql) {
    Plan plan = {0};
    Parser parser = make_parser(sql);

    skip_spaces(&parser);

    if (!ends_with_semicolon(parser.cursor)) {
        set_error(&plan, "SQL은 ;로 끝나야 합니다");
        return plan;
    }

    if (starts_with(parser.cursor, "select")) {
        return parse_select(parser.cursor);
    }

    if (starts_with(parser.cursor, "insert")) {
        return parse_insert(parser.cursor);
    }

    set_error(&plan, "지원하지 않는 SQL");
    return plan;
}

/* 3.2/3.3 SELECT 조회 파싱: 전체 조회와 where id 조건 조회 계획을 만든다. */
static Plan parse_select(const char *sql) {
    Parser parser = make_parser(sql);
    char table_name[MAX_TABLE_NAME_SIZE];
    SelectCondition condition = {0};

    expect_text(&parser, "select");
    skip_spaces(&parser);
    expect_char(&parser, '*');
    skip_spaces(&parser);
    expect_text(&parser, "from");
    skip_spaces(&parser);
    read_name(&parser, table_name, sizeof(table_name));
    skip_spaces(&parser);

    if (!parser.has_error && *parser.cursor != ';') {
        if (!starts_with(parser.cursor, "where")) {
            set_parser_error(&parser, "지원하지 않는 SELECT 문법입니다");
        } else {
            expect_text(&parser, "where");
            skip_spaces(&parser);
            if (!parser.has_error && !starts_with(parser.cursor, "id")) {
                set_parser_error(&parser, "지원하지 않는 SELECT 문법입니다");
            }
            expect_text(&parser, "id");
            skip_spaces(&parser);

            if (!parser.has_error && *parser.cursor == '=') {
                condition.type = SELECT_CONDITION_ID_EQUALS;
                expect_char(&parser, '=');
                skip_spaces(&parser);
                read_integer(&parser, &condition.id_value);
            } else if (!parser.has_error && starts_with(parser.cursor, "between")) {
                condition.type = SELECT_CONDITION_ID_BETWEEN;
                expect_text(&parser, "between");
                skip_spaces(&parser);
                read_integer(&parser, &condition.id_start_value);
                skip_spaces(&parser);
                if (!parser.has_error && !starts_with(parser.cursor, "and")) {
                    set_parser_error(&parser, "지원하지 않는 SELECT 문법입니다");
                }
                expect_text(&parser, "and");
                skip_spaces(&parser);
                read_integer(&parser, &condition.id_end_value);
            } else if (!parser.has_error) {
                set_parser_error(&parser, "지원하지 않는 SELECT 문법입니다");
            }

            skip_spaces(&parser);
            if (!parser.has_error && *parser.cursor != ';') {
                set_parser_error(&parser, "지원하지 않는 SELECT 문법입니다");
            }
        }
    }

    expect_char(&parser, ';');
    skip_spaces(&parser);
    expect_end(&parser);

    return build_select_plan(&parser, table_name, condition);
}

/* 3.4 INSERT 값 파싱: 테이블 이름과 values 목록을 실행 계획으로 만든다. */
static Plan parse_insert(const char *sql) {
    Parser parser = make_parser(sql);
    char table_name[MAX_TABLE_NAME_SIZE];
    char values_text[MAX_INPUT_SIZE];

    expect_text(&parser, "insert");
    skip_spaces(&parser);
    expect_text(&parser, "into");
    skip_spaces(&parser);
    read_name(&parser, table_name, sizeof(table_name));
    skip_spaces(&parser);
    expect_text(&parser, "values");
    skip_spaces(&parser);
    read_until_semicolon(&parser, values_text, sizeof(values_text));
    expect_char(&parser, ';');
    skip_spaces(&parser);
    expect_end(&parser);

    return build_insert_plan(&parser, table_name, values_text);
}

static Parser make_parser(const char *sql) {
    Parser parser = {0};

    parser.cursor = sql;
    return parser;
}

/* 내부 구현: SQL이 지정한 키워드로 시작하는지 확인한다. */
static int starts_with(const char *text, const char *prefix) {
    for (size_t i = 0; prefix[i] != '\0'; i++) {
        if (!chars_equal_ignore_case(text[i], prefix[i])) {
            return 0;
        }
    }

    return 1;
}

/* 내부 구현: ASCII 문자 2개를 대소문자 구분 없이 비교한다. */
static int chars_equal_ignore_case(char left, char right) {
    return to_lower_ascii(left) == to_lower_ascii(right);
}

/* 내부 구현: ASCII 대문자를 소문자로 바꾼다. INSERT 값 자체에는 적용하지 않는다. */
static char to_lower_ascii(char value) {
    if (value >= 'A' && value <= 'Z') {
        return (char) (value - 'A' + 'a');
    }

    return value;
}

/* 내부 구현: SQL이 공백을 제외하고 세미콜론으로 끝나는지 확인한다. */
static int ends_with_semicolon(const char *text) {
    const char *end = text + strlen(text);

    while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) {
        end--;
    }

    return end > text && end[-1] == ';';
}

/* 내부 구현: 파싱 커서를 다음 의미 있는 문자 위치로 옮긴다. */
static void skip_spaces(Parser *parser) {
    if (parser->has_error) {
        return;
    }

    while (*parser->cursor == ' ' || *parser->cursor == '\t' || *parser->cursor == '\n' ||
           *parser->cursor == '\r') {
        parser->cursor++;
    }
}

/* 내부 구현: 현재 커서가 지정한 문자열이면 그만큼 앞으로 이동한다. */
static void expect_text(Parser *parser, const char *expected) {
    size_t length = strlen(expected);

    if (parser->has_error) {
        return;
    }

    if (!starts_with(parser->cursor, expected)) {
        set_parser_error(parser, "지원하지 않는 SQL");
        return;
    }

    parser->cursor += length;
}

/* 내부 구현: 현재 커서가 지정한 문자이면 한 칸 앞으로 이동한다. */
static void expect_char(Parser *parser, char expected) {
    if (parser->has_error) {
        return;
    }

    if (*parser->cursor != expected) {
        set_parser_error(parser, "지원하지 않는 SQL");
        return;
    }

    parser->cursor++;
}

/* 내부 구현: 파싱이 입력 끝까지 도달했는지 확인한다. */
static void expect_end(Parser *parser) {
    if (parser->has_error) {
        return;
    }

    if (*parser->cursor != '\0') {
        set_parser_error(parser, "지원하지 않는 SQL");
    }
}

/* 내부 구현: 현재 커서에서 테이블 이름 하나를 읽는다. */
static void read_name(Parser *parser, char *buffer, size_t buffer_size) {
    size_t length = 0;

    buffer[0] = '\0';
    if (parser->has_error) {
        return;
    }

    while (parser->cursor[length] != '\0' && parser->cursor[length] != ' ' && parser->cursor[length] != '\t' &&
           parser->cursor[length] != '\n' && parser->cursor[length] != '\r' && parser->cursor[length] != ';') {
        if (length + 1 >= buffer_size) {
            set_parser_error(parser, "지원하지 않는 SQL");
            return;
        }
        length++;
    }

    if (length == 0) {
        set_parser_error(parser, "지원하지 않는 SQL");
        return;
    }

    snprintf(buffer, buffer_size, "%.*s", (int) length, parser->cursor);
    parser->cursor += length;
}

/* 내부 구현: 현재 커서에서 정수 하나를 읽는다. */
static void read_integer(Parser *parser, int *value) {
    char buffer[MAX_VALUE_SIZE];
    char *end;
    long parsed;
    size_t length = 0;

    *value = 0;
    if (parser->has_error) {
        return;
    }

    if (parser->cursor[length] == '-') {
        length++;
    }

    while (parser->cursor[length] >= '0' && parser->cursor[length] <= '9') {
        if (length + 1 >= sizeof(buffer)) {
            set_parser_error(parser, "지원하지 않는 SELECT 문법입니다");
            return;
        }
        length++;
    }

    if (length == 0 || (length == 1 && parser->cursor[0] == '-')) {
        set_parser_error(parser, "지원하지 않는 SELECT 문법입니다");
        return;
    }

    snprintf(buffer, sizeof(buffer), "%.*s", (int) length, parser->cursor);
    parsed = strtol(buffer, &end, 10);
    if (*end != '\0' || parsed < INT_MIN || parsed > INT_MAX) {
        set_parser_error(parser, "지원하지 않는 SELECT 문법입니다");
        return;
    }

    *value = (int) parsed;
    parser->cursor += length;
}

/* 내부 구현: 현재 커서에서 세미콜론 전까지의 값을 읽는다. */
static void read_until_semicolon(Parser *parser, char *buffer, size_t buffer_size) {
    size_t length = 0;

    buffer[0] = '\0';
    if (parser->has_error) {
        return;
    }

    while (parser->cursor[length] != '\0' && parser->cursor[length] != ';') {
        if (length + 1 >= buffer_size) {
            set_parser_error(parser, "지원하지 않는 SQL");
            return;
        }
        length++;
    }

    if (length == 0 || parser->cursor[length] != ';') {
        set_parser_error(parser, "지원하지 않는 SQL");
        return;
    }

    snprintf(buffer, buffer_size, "%.*s", (int) length, parser->cursor);
    parser->cursor += length;
}

static Plan build_select_plan(Parser *parser, const char *table_name, SelectCondition condition) {
    Plan plan = {0};
    const TableMetadata *table;

    if (parser->has_error) {
        return parser->error_plan;
    }

    table = find_table(table_name);
    if (table == NULL) {
        set_error(&plan, "존재하지 않는 테이블입니다");
        return plan;
    }

    plan.type = QUERY_SELECT;
    plan.condition = condition;
    snprintf(plan.table_name, sizeof(plan.table_name), "%s", table->name);
    return plan;
}

static Plan build_insert_plan(Parser *parser, const char *table_name, char *values_text) {
    Plan plan = {0};
    char *values;
    char *token;
    const TableMetadata *table;

    if (parser->has_error) {
        return parser->error_plan;
    }

    values = trim(values_text);
    if (*values == '(') {
        values++;
    }
    values = trim(values);
    if (strlen(values) > 0 && values[strlen(values) - 1] == ')') {
        values[strlen(values) - 1] = '\0';
    }
    values = trim(values);

    table = find_table(table_name);
    if (table == NULL) {
        set_error(&plan, "존재하지 않는 테이블입니다");
        return plan;
    }

    token = strtok(values, ",");
    while (token != NULL && plan.value_count < MAX_VALUES) {
        char *value = trim(token);

        if (!is_ascii_text(value)) {
            set_error(&plan, "ASCII 텍스트만 입력할 수 있습니다");
            return plan;
        }

        snprintf(plan.values[plan.value_count], sizeof(plan.values[plan.value_count]), "%s", value);
        plan.value_count++;
        token = strtok(NULL, ",");
    }

    if (token != NULL || plan.value_count != table->column_count) {
        set_error(&plan, "컬럼 개수와 값 개수가 맞지 않습니다");
        return plan;
    }

    plan.type = QUERY_INSERT;
    snprintf(plan.table_name, sizeof(plan.table_name), "%s", table->name);
    return plan;
}

/* 내부 구현: Plan을 실패 상태로 바꾸고 사용자에게 보여줄 오류 메시지를 저장한다. */
static void set_error(Plan *plan, const char *message) {
    plan->type = QUERY_INVALID;
    snprintf(plan->error_message, sizeof(plan->error_message), "%s", message);
}

static void set_parser_error(Parser *parser, const char *message) {
    if (parser->has_error) {
        return;
    }

    parser->has_error = 1;
    set_error(&parser->error_plan, message);
}

/* 내부 구현: INSERT 값에 ASCII 범위를 벗어나는 문자가 있는지 확인한다. */
static int is_ascii_text(const char *text) {
    const unsigned char *cursor = (const unsigned char *) text;

    while (*cursor != '\0') {
        if (*cursor > 127) {
            return 0;
        }
        cursor++;
    }

    return 1;
}

/* 내부 구현: 파싱 중 잘라낸 문자열 조각의 앞뒤 공백과 개행을 제거한다. */
static char *trim(char *text) {
    char *end;

    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') {
        text++;
    }

    if (*text == '\0') {
        return text;
    }

    end = text + strlen(text) - 1;
    while (end > text && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        *end = '\0';
        end--;
    }

    return text;
}
