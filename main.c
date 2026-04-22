#include <stdio.h>
#include <string.h>

#include "mini_db.h"

#ifndef PROJECT_ROOT_DIR
#define PROJECT_ROOT_DIR "."
#endif

#define PROJECT_CSV_PATH(path) PROJECT_ROOT_DIR "/" path

/* 내부 구현: main 흐름을 구성하는 private 함수 목록이다. */
static int prepare_database(char *error_message, size_t error_size);
static void shutdown_database(void);
static int run_repl(void);
static int strings_equal_ignore_case(const char *left, const char *right);
static char to_lower_ascii(char value);
static char *trim(char *text);
static void print_init(void);

/* 5.1 테이블 이름 매핑: 테이블별 컬럼, 데이터 파일, 인덱스 파일, row 크기 정보다. */
static const TableMetadata GLOBAL_TABLES[] = {
    {"users", {"id", "name"}, 2, PROJECT_CSV_PATH("data/users.csv"), PROJECT_CSV_PATH("data/users.idx"), ROW_SIZE},
    {"posts", {"id", "title"}, 2, PROJECT_CSV_PATH("data/posts.csv"), PROJECT_CSV_PATH("data/posts.idx"), ROW_SIZE},
};

static const int GLOBAL_TABLE_COUNT = sizeof(GLOBAL_TABLES) / sizeof(GLOBAL_TABLES[0]);

int main(void) {
    char error_message[MAX_ERROR_SIZE];
    int result;

    print_init(); // 그냥 꾸며주는 코드
    if (prepare_database(error_message, sizeof(error_message)) != 0) {
        printf("%s\n", error_message);
        shutdown_database();
        return 1;
    }

    result = run_repl();
    shutdown_database();
    return result;
}

/* 1. 프로그램 시작 / 인덱스 준비: REPL 전에 모든 테이블 인덱스를 사용할 수 있게 만든다. */
static int prepare_database(char *error_message, size_t error_size) {
    for (int i = 0; i < GLOBAL_TABLE_COUNT; i++) {
        if (db_index_open_table(&GLOBAL_TABLES[i], error_message, error_size) != 0) {
            return -1;
        }
    }

    return 0;
}

/* 2.3 특수 명령 처리: 종료 시 열린 인덱스 자원을 정리한다. */
static void shutdown_database(void) {
    db_index_shutdown_all();
}

/* 2. REPL SQL 입력 처리: SQL 한 줄을 받아 파싱과 실행으로 넘긴다. */
static int run_repl(void) {
    char input[MAX_INPUT_SIZE];

    while (1) {
        char *sql;
        Plan plan;

        /* 2.1 프롬프트 출력 */
        printf("mini-db> ");
        if (fgets(input, sizeof(input), stdin) == NULL) {
            printf("\n");
            return 0;
        }

        sql = trim(input);

        /* 2.3 특수 명령 처리 */
        if (strcmp(sql, ".exit") == 0) {
            return 0;
        }

        /* 3. SQL 파싱 */
        plan = parse_sql(sql);
        if (plan.type == QUERY_INVALID) {
            printf("%s\n", plan.error_message);
            continue;
        }

        /* 4.1 실행 분기 */
        execute_plan(&plan);
    }
}

/* 5.1 테이블 이름 매핑: 테이블 이름을 메타데이터로 바꾼다. */
const TableMetadata *find_table(const char *table_name) {
    for (int i = 0; i < GLOBAL_TABLE_COUNT; i++) {
        if (strings_equal_ignore_case(GLOBAL_TABLES[i].name, table_name)) {
            return &GLOBAL_TABLES[i];
        }
    }

    return NULL;
}

/* 내부 구현: 테이블 이름을 대소문자 구분 없이 비교한다. */
static int strings_equal_ignore_case(const char *left, const char *right) {
    while (*left != '\0' && *right != '\0') {
        if (to_lower_ascii(*left) != to_lower_ascii(*right)) {
            return 0;
        }

        left++;
        right++;
    }

    return *left == '\0' && *right == '\0';
}

/* 내부 구현: ASCII 대문자를 소문자로 바꾼다. */
static char to_lower_ascii(char value) {
    if (value >= 'A' && value <= 'Z') {
        return (char) (value - 'A' + 'a');
    }

    return value;
}

/* 내부 구현: 입력 문자열의 앞뒤 공백과 개행을 제거한다. */
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

static void print_init(void)
{
    printf(" __  __ ___ _   _ ___     ____  ____  \n");
    printf("|  \\/  |_ _| \\ | |_ _|   |  _ \\| __ ) \n");
    printf("| |\\/| || ||  \\| || |____| | | |  _ \\ \n");
    printf("| |  | || || |\\  || |____| |_| | |_) |\n");
    printf("|_|  |_|___|_| \\_|___|   |____/|____/ \n");
    printf("\n");
    printf("Special commands start with '.'. Current command: .exit\n");
    printf("\n");
}
