#include "tokenizer.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int tokenizer_append_token(Token **tokens, int *count, int *capacity,
                                  TokenType type, const char *value,
                                  char *error, size_t error_size) {
    Token *new_tokens;

    if (tokens == NULL || count == NULL || capacity == NULL || value == NULL) {
        return FAILURE;
    }

    if (*tokens == NULL) {
        *capacity = INITIAL_TOKEN_CAPACITY;
        *tokens = (Token *)malloc((size_t)(*capacity) * sizeof(Token));
        if (*tokens == NULL) {
            utils_set_error(error, error_size, "Failed to allocate memory for tokens.");
            return FAILURE;
        }
    } else if (*count >= *capacity) {
        *capacity *= 2;
        new_tokens = (Token *)realloc(*tokens, (size_t)(*capacity) * sizeof(Token));
        if (new_tokens == NULL) {
            utils_set_error(error, error_size, "Failed to allocate memory for tokens.");
            return FAILURE;
        }
        *tokens = new_tokens;
    }

    (*tokens)[*count].type = type;
    if (utils_safe_strcpy((*tokens)[*count].value, sizeof((*tokens)[*count].value),
                          value) != SUCCESS) {
        utils_set_error(error, error_size, "Token value is too long.");
        return FAILURE;
    }

    (*count)++;
    return SUCCESS;
}

static int tokenizer_read_word(const char *sql, size_t *index, char *buffer,
                               size_t buffer_size) {
    size_t length;

    length = 0;
    while (sql[*index] != '\0' &&
           (isalnum((unsigned char)sql[*index]) || sql[*index] == '_')) {
        if (length + 1 >= buffer_size) {
            return FAILURE;
        }
        buffer[length++] = sql[*index];
        (*index)++;
    }
    buffer[length] = '\0';
    return SUCCESS;
}

static int tokenizer_read_string(const char *sql, size_t *index, char *buffer,
                                 size_t buffer_size) {
    size_t length;

    length = 0;
    (*index)++;
    while (sql[*index] != '\0') {
        if (sql[*index] == '\'') {
            if (sql[*index + 1] == '\'') {
                if (length + 1 >= buffer_size) {
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
            return FAILURE;
        }

        buffer[length++] = sql[*index];
        (*index)++;
    }

    return FAILURE;
}

static int tokenizer_read_number(const char *sql, size_t *index, char *buffer,
                                 size_t buffer_size) {
    size_t length;

    length = 0;
    if (sql[*index] == '-' || sql[*index] == '+') {
        if (length + 1 >= buffer_size) {
            return FAILURE;
        }
        buffer[length++] = sql[*index];
        (*index)++;
    }

    while (isdigit((unsigned char)sql[*index])) {
        if (length + 1 >= buffer_size) {
            return FAILURE;
        }
        buffer[length++] = sql[*index];
        (*index)++;
    }

    buffer[length] = '\0';
    return SUCCESS;
}

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

Token *tokenizer_tokenize_with_error(const char *sql, int *token_count,
                                     char *error, size_t error_size) {
    Token *tokens;
    int count;
    int capacity;
    char upper_buffer[MAX_TOKEN_VALUE];
    char token_buffer[MAX_TOKEN_VALUE];
    char *working_sql;
    size_t i;

    if (sql == NULL || token_count == NULL) {
        return NULL;
    }

    *token_count = 0;
    utils_set_error(error, error_size, "");

    working_sql = utils_strdup(sql);
    if (working_sql == NULL) {
        utils_set_error(error, error_size, "Failed to allocate memory.");
        return NULL;
    }

    utils_trim(working_sql);
    if (working_sql[0] == '\0') {
        free(working_sql);
        utils_set_error(error, error_size, "Empty SQL statement.");
        return NULL;
    }

    tokens = NULL;
    count = 0;
    capacity = 0;
    i = 0;
    while (working_sql[i] != '\0') {
        if (isspace((unsigned char)working_sql[i])) {
            i++;
            continue;
        }

        if (working_sql[i] == '(') {
            if (tokenizer_append_token(&tokens, &count, &capacity, TOKEN_LPAREN, "(",
                                       error, error_size) != SUCCESS) {
                free(tokens);
                free(working_sql);
                return NULL;
            }
            i++;
            continue;
        }

        if (working_sql[i] == ')') {
            if (tokenizer_append_token(&tokens, &count, &capacity, TOKEN_RPAREN, ")",
                                       error, error_size) != SUCCESS) {
                free(tokens);
                free(working_sql);
                return NULL;
            }
            i++;
            continue;
        }

        if (working_sql[i] == ',') {
            if (tokenizer_append_token(&tokens, &count, &capacity, TOKEN_COMMA, ",",
                                       error, error_size) != SUCCESS) {
                free(tokens);
                free(working_sql);
                return NULL;
            }
            i++;
            continue;
        }

        if (working_sql[i] == ';') {
            if (tokenizer_append_token(&tokens, &count, &capacity, TOKEN_SEMICOLON, ";",
                                       error, error_size) != SUCCESS) {
                free(tokens);
                free(working_sql);
                return NULL;
            }
            i++;
            continue;
        }

        if (working_sql[i] == '*') {
            if (tokenizer_append_token(&tokens, &count, &capacity,
                                       TOKEN_IDENTIFIER, "*",
                                       error, error_size) != SUCCESS) {
                free(tokens);
                free(working_sql);
                return NULL;
            }
            i++;
            continue;
        }

        if (working_sql[i] == '\'') {
            if (tokenizer_read_string(working_sql, &i, token_buffer,
                                      sizeof(token_buffer)) != SUCCESS) {
                utils_set_error(error, error_size, "Unterminated string literal.");
                free(tokens);
                free(working_sql);
                return NULL;
            }

            if (tokenizer_append_token(&tokens, &count, &capacity,
                                       TOKEN_STR_LITERAL, token_buffer,
                                       error, error_size) != SUCCESS) {
                free(tokens);
                free(working_sql);
                return NULL;
            }
            continue;
        }

        if (working_sql[i] == '!' || working_sql[i] == '<' ||
            working_sql[i] == '>' || working_sql[i] == '=') {
            token_buffer[0] = working_sql[i];
            token_buffer[1] = '\0';
            if ((working_sql[i] == '!' || working_sql[i] == '<' ||
                 working_sql[i] == '>') &&
                working_sql[i + 1] == '=') {
                token_buffer[1] = '=';
                token_buffer[2] = '\0';
                i += 2;
            } else {
                i++;
            }

            if (tokenizer_append_token(&tokens, &count, &capacity,
                                       TOKEN_OPERATOR, token_buffer,
                                       error, error_size) != SUCCESS) {
                free(tokens);
                free(working_sql);
                return NULL;
            }
            continue;
        }

        if (tokenizer_is_numeric_start(working_sql, i)) {
            if (tokenizer_read_number(working_sql, &i, token_buffer,
                                      sizeof(token_buffer)) != SUCCESS) {
                utils_set_error(error, error_size, "Integer literal is too long.");
                free(tokens);
                free(working_sql);
                return NULL;
            }

            if (tokenizer_append_token(&tokens, &count, &capacity,
                                       TOKEN_INT_LITERAL, token_buffer,
                                       error, error_size) != SUCCESS) {
                free(tokens);
                free(working_sql);
                return NULL;
            }
            continue;
        }

        if (isalpha((unsigned char)working_sql[i]) || working_sql[i] == '_') {
            if (tokenizer_read_word(working_sql, &i, token_buffer,
                                    sizeof(token_buffer)) != SUCCESS) {
                utils_set_error(error, error_size, "Identifier is too long.");
                free(tokens);
                free(working_sql);
                return NULL;
            }

            if (utils_is_sql_keyword(token_buffer)) {
                if (utils_to_upper_copy(token_buffer, upper_buffer,
                                        sizeof(upper_buffer)) != SUCCESS) {
                    utils_set_error(error, error_size, "Keyword token is too long.");
                    free(tokens);
                    free(working_sql);
                    return NULL;
                }
                if (tokenizer_append_token(&tokens, &count, &capacity,
                                           TOKEN_KEYWORD, upper_buffer,
                                           error, error_size) != SUCCESS) {
                    free(tokens);
                    free(working_sql);
                    return NULL;
                }
            } else {
                if (tokenizer_append_token(&tokens, &count, &capacity,
                                           TOKEN_IDENTIFIER, token_buffer,
                                           error, error_size) != SUCCESS) {
                    free(tokens);
                    free(working_sql);
                    return NULL;
                }
            }
            continue;
        }

        token_buffer[0] = working_sql[i];
        token_buffer[1] = '\0';
        if (tokenizer_append_token(&tokens, &count, &capacity, TOKEN_UNKNOWN,
                                   token_buffer, error, error_size) != SUCCESS) {
            free(tokens);
            free(working_sql);
            return NULL;
        }
        i++;
    }

    *token_count = count;
    free(working_sql);
    return tokens;
}

Token *tokenizer_tokenize(const char *sql, int *token_count) {
    return tokenizer_tokenize_with_error(sql, token_count, NULL, 0);
}

void tokenizer_cleanup_cache(void) {
    return;
}

int tokenizer_get_cache_entry_count(void) {
    return 0;
}

int tokenizer_get_cache_hit_count(void) {
    return 0;
}

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
