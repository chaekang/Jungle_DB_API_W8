#ifndef TOKENIZER_H
#define TOKENIZER_H

#include "utils.h"

typedef enum {
    TOKEN_KEYWORD,
    TOKEN_IDENTIFIER,
    TOKEN_INT_LITERAL,
    TOKEN_STR_LITERAL,
    TOKEN_OPERATOR,
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_COMMA,
    TOKEN_SEMICOLON,
    TOKEN_UNKNOWN
} TokenType;

typedef struct {
    TokenType type;
    char value[MAX_TOKEN_VALUE];
} Token;

Token *tokenizer_tokenize(const char *sql, int *token_count);
Token *tokenizer_tokenize_with_error(const char *sql, int *token_count,
                                     char *error, size_t error_size);
void tokenizer_cleanup_cache(void);
int tokenizer_get_cache_entry_count(void);
int tokenizer_get_cache_hit_count(void);
const char *tokenizer_token_type_name(TokenType type);

#endif
