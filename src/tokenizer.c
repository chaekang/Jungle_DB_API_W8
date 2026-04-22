#include "tokenizer.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static __thread char tokenizer_last_error[256] = "Failed to tokenize SQL.";

static void tokenizer_set_error(const char *message) {
    if (message == NULL) {
        return;
    }

    utils_safe_strcpy(tokenizer_last_error, sizeof(tokenizer_last_error), message);
}

/*
 * 늘어나는 토큰 배열에 토큰 하나를 추가한다.
 * tokens에 정상 저장되면 SUCCESS를 반환한다.
 */
static int tokenizer_append_token(Token **tokens, int *count, int *capacity,
                                    TokenType type, const char *value) {
    Token *new_tokens;

    if (tokens == NULL || count == NULL || capacity == NULL || value == NULL) {
        return FAILURE;
    }

    if (*tokens == NULL) {
        *capacity = INITIAL_TOKEN_CAPACITY;
        *tokens = (Token *)malloc((size_t)(*capacity) * sizeof(Token));
        if (*tokens == NULL) {
            tokenizer_set_error("Failed to allocate memory for tokens.");
            return FAILURE;
        }
    } else if (*count >= *capacity) {
        *capacity *= 2;
        new_tokens = (Token *)realloc(*tokens, (size_t)(*capacity) * sizeof(Token));
        if (new_tokens == NULL) {
            tokenizer_set_error("Failed to allocate memory for tokens.");
            return FAILURE;
        }
        *tokens = new_tokens;
    }

    (*tokens)[*count].type = type;
    if (utils_safe_strcpy((*tokens)[*count].value, sizeof((*tokens)[*count].value),
                          value) != SUCCESS) {
        tokenizer_set_error("Token value is too long.");
        return FAILURE;
    }

    (*count)++;
    return SUCCESS;
}

/*
 * 현재 위치에서 식별자 또는 키워드 후보 하나를 읽는다.
 * 성공 시 index는 읽은 뒤 위치로 이동한다.
 */
static int tokenizer_read_word(const char *sql, size_t *index, char *buffer,
                                 size_t buffer_size) {
    size_t length;

    length = 0;
    while (sql[*index] != '\0' &&
           (isalnum((unsigned char)sql[*index]) || sql[*index] == '_')) {
        if (length + 1 >= buffer_size) {
            tokenizer_set_error("Identifier is too long.");
            return FAILURE;
        }
        buffer[length++] = sql[*index];
        (*index)++;
    }
    buffer[length] = '\0';
    return SUCCESS;
}

/*
 * 작은따옴표로 감싼 SQL 문자열 리터럴 하나를 읽는다.
 * 바깥 따옴표는 제외하고, 내부의 연속 작은따옴표 이스케이프도 처리한다.
 */
static int tokenizer_read_string(const char *sql, size_t *index, char *buffer,
                                   size_t buffer_size) {
    size_t length;

    length = 0;
    (*index)++;
    while (sql[*index] != '\0') {
        if (sql[*index] == '\'') {
            if (sql[*index + 1] == '\'') {
                if (length + 1 >= buffer_size) {
                    tokenizer_set_error("String literal is too long.");
                    return FAILURE;
                }
                buffer[length++] = '\'';
                *index += 2;
                continue;
            }
            (*index)++;
            buffer[length] = '\0';
            return SUCCESS;
        }

        if (length + 1 >= buffer_size) {
            tokenizer_set_error("String literal is too long.");
            return FAILURE;
        }

        buffer[length++] = sql[*index];
        (*index)++;
    }

    tokenizer_set_error("Unterminated string literal.");
    return FAILURE;
}

/*
 * 부호를 포함할 수 있는 정수 리터럴 하나를 읽어 buffer에 복사한다.
 */
static int tokenizer_read_number(const char *sql, size_t *index, char *buffer,
                                   size_t buffer_size) {
    size_t length;

    length = 0;
    if (sql[*index] == '-' || sql[*index] == '+') {
        if (length + 1 >= buffer_size) {
            tokenizer_set_error("Integer literal is too long.");
            return FAILURE;
        }
        buffer[length++] = sql[*index];
        (*index)++;
    }

    while (isdigit((unsigned char)sql[*index])) {
        if (length + 1 >= buffer_size) {
            tokenizer_set_error("Integer literal is too long.");
            return FAILURE;
        }
        buffer[length++] = sql[*index];
        (*index)++;
    }

    buffer[length] = '\0';
    return SUCCESS;
}

/*
 * sql[index]가 정수 리터럴 시작이면 1, 아니면 0을 반환한다.
 */
static int tokenizer_is_numeric_start(const char *sql, size_t index) {
    if (isdigit((unsigned char)sql[index])) {
        return 1;
    }

    if ((sql[index] == '-' || sql[index] == '+') &&
        isdigit((unsigned char)sql[index + 1])) {
        return 1;
    }

    return 0;
}

/*
 * 이미 trim된 SQL 문 하나를 새 토큰 배열로 분해한다.
 * 반환된 배열은 호출자가 소유한다.
 */
static Token *tokenizer_tokenize_sql(const char *sql, int *token_count) {
    Token *tokens;
    int count;
    int capacity;
    char upper_buffer[MAX_TOKEN_VALUE];
    char token_buffer[MAX_TOKEN_VALUE];
    size_t i;

    if (sql == NULL || token_count == NULL) {
        return NULL;
    }

    *token_count = 0;
    tokens = NULL;
    count = 0;
    capacity = 0;
    i = 0;
    while (sql[i] != '\0') {
        if (isspace((unsigned char)sql[i])) {
            i++;
            continue;
        }

        if (sql[i] == '(') {
            if (tokenizer_append_token(&tokens, &count, &capacity, TOKEN_LPAREN,
                                         "(") != SUCCESS) {
                free(tokens);
                return NULL;
            }
            i++;
            continue;
        }

        if (sql[i] == ')') {
            if (tokenizer_append_token(&tokens, &count, &capacity, TOKEN_RPAREN,
                                         ")") != SUCCESS) {
                free(tokens);
                return NULL;
            }
            i++;
            continue;
        }

        if (sql[i] == ',') {
            if (tokenizer_append_token(&tokens, &count, &capacity, TOKEN_COMMA,
                                         ",") != SUCCESS) {
                free(tokens);
                return NULL;
            }
            i++;
            continue;
        }

        if (sql[i] == ';') {
            if (tokenizer_append_token(&tokens, &count, &capacity,
                                         TOKEN_SEMICOLON, ";") != SUCCESS) {
                free(tokens);
                return NULL;
            }
            i++;
            continue;
        }

        if (sql[i] == '*') {
            if (tokenizer_append_token(&tokens, &count, &capacity,
                                         TOKEN_IDENTIFIER, "*") != SUCCESS) {
                free(tokens);
                return NULL;
            }
            i++;
            continue;
        }

        if (sql[i] == '\'') {
            if (tokenizer_read_string(sql, &i, token_buffer,
                                        sizeof(token_buffer)) != SUCCESS) {
                free(tokens);
                return NULL;
            }

            if (tokenizer_append_token(&tokens, &count, &capacity,
                                         TOKEN_STR_LITERAL, token_buffer) != SUCCESS) {
                free(tokens);
                return NULL;
            }
            continue;
        }

        if (sql[i] == '!' || sql[i] == '<' || sql[i] == '>' || sql[i] == '=') {
            token_buffer[0] = sql[i];
            token_buffer[1] = '\0';
            if ((sql[i] == '!' || sql[i] == '<' || sql[i] == '>') &&
                sql[i + 1] == '=') {
                token_buffer[1] = '=';
                token_buffer[2] = '\0';
                i += 2;
            } else {
                i++;
            }

            if (tokenizer_append_token(&tokens, &count, &capacity,
                                         TOKEN_OPERATOR, token_buffer) != SUCCESS) {
                free(tokens);
                return NULL;
            }
            continue;
        }

        if (tokenizer_is_numeric_start(sql, i)) {
            if (tokenizer_read_number(sql, &i, token_buffer,
                                        sizeof(token_buffer)) != SUCCESS) {
                free(tokens);
                return NULL;
            }

            if (tokenizer_append_token(&tokens, &count, &capacity,
                                         TOKEN_INT_LITERAL, token_buffer) != SUCCESS) {
                free(tokens);
                return NULL;
            }
            continue;
        }

        if (isalpha((unsigned char)sql[i]) || sql[i] == '_') {
            if (tokenizer_read_word(sql, &i, token_buffer,
                                      sizeof(token_buffer)) != SUCCESS) {
                free(tokens);
                return NULL;
            }

            if (utils_is_sql_keyword(token_buffer)) {
                if (utils_to_upper_copy(token_buffer, upper_buffer,
                                        sizeof(upper_buffer)) != SUCCESS) {
                    free(tokens);
                    return NULL;
                }
                if (tokenizer_append_token(&tokens, &count, &capacity,
                                             TOKEN_KEYWORD, upper_buffer) != SUCCESS) {
                    free(tokens);
                    return NULL;
                }
            } else {
                if (tokenizer_append_token(&tokens, &count, &capacity,
                                             TOKEN_IDENTIFIER, token_buffer) != SUCCESS) {
                    free(tokens);
                    return NULL;
                }
            }
            continue;
        }

        token_buffer[0] = sql[i];
        token_buffer[1] = '\0';
        if (tokenizer_append_token(&tokens, &count, &capacity, TOKEN_UNKNOWN,
                                     token_buffer) != SUCCESS) {
            free(tokens);
            return NULL;
        }
        i++;
    }

    *token_count = count;
    return tokens;
}

/*
 * SQL 문 하나를 정규화한 뒤 호출자가 소유하는 토큰 배열을 반환한다.
 * 서버 worker 여러 개가 동시에 호출해도 공유 상태가 없도록 캐시는 쓰지 않는다.
 */
Token *tokenizer_tokenize(const char *sql, int *token_count) {
    char *working_sql;
    Token *tokens;
    int parsed_token_count;

    if (sql == NULL || token_count == NULL) {
        tokenizer_set_error("SQL is empty.");
        return NULL;
    }

    tokenizer_set_error("Failed to tokenize SQL.");
    *token_count = 0;
    working_sql = utils_strdup(sql);
    if (working_sql == NULL) {
        tokenizer_set_error("Failed to allocate memory for SQL.");
        return NULL;
    }

    utils_trim(working_sql);
    if (working_sql[0] == '\0') {
        tokenizer_set_error("SQL is empty.");
        free(working_sql);
        return NULL;
    }

    tokens = tokenizer_tokenize_sql(working_sql, &parsed_token_count);
    if (tokens == NULL) {
        free(working_sql);
        return NULL;
    }

    *token_count = parsed_token_count;
    free(working_sql);
    return tokens;
}

/*
 * 예전 캐시 API와의 호환성을 위한 no-op이다.
 * 토크나이저는 더 이상 전역 캐시를 보관하지 않는다.
 */
void tokenizer_cleanup_cache(void) {
}

/*
 * 캐시를 사용하지 않으므로 항상 0을 반환한다.
 */
int tokenizer_get_cache_entry_count(void) {
    return 0;
}

/*
 * 캐시를 사용하지 않으므로 항상 0을 반환한다.
 */
int tokenizer_get_cache_hit_count(void) {
    return 0;
}

/*
 * 토큰 타입 enum 값을 디버깅이나 테스트용 문자열로 바꾼다.
 */
const char *tokenizer_token_type_name(TokenType type) {
    switch (type) {
        case TOKEN_KEYWORD:
            return "KEYWORD";
        case TOKEN_IDENTIFIER:
            return "IDENTIFIER";
        case TOKEN_INT_LITERAL:
            return "INT_LITERAL";
        case TOKEN_STR_LITERAL:
            return "STR_LITERAL";
        case TOKEN_OPERATOR:
            return "OPERATOR";
        case TOKEN_LPAREN:
            return "LPAREN";
        case TOKEN_RPAREN:
            return "RPAREN";
        case TOKEN_COMMA:
            return "COMMA";
        case TOKEN_SEMICOLON:
            return "SEMICOLON";
        case TOKEN_UNKNOWN:
        default:
            return "UNKNOWN";
    }
}

const char *tokenizer_get_last_error(void) {
    return tokenizer_last_error;
}
