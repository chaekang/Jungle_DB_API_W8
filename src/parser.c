#include "parser.h"

#include <string.h>

static int parser_is_token(const Token *tokens, int token_count, int index,
                           TokenType type, const char *value) {
    if (tokens == NULL || index < 0 || index >= token_count) {
        return 0;
    }

    if (tokens[index].type != type) {
        return 0;
    }

    if (value == NULL) {
        return 1;
    }

    return strcmp(tokens[index].value, value) == 0;
}

static int parser_expect_keyword(const Token *tokens, int token_count, int *index,
                                 const char *keyword, char *error,
                                 size_t error_size) {
    if (!parser_is_token(tokens, token_count, *index, TOKEN_KEYWORD, keyword)) {
        utils_set_error(error, error_size, "Unexpected SQL syntax.");
        return FAILURE;
    }

    (*index)++;
    return SUCCESS;
}

static int parser_expect_identifier(const Token *tokens, int token_count, int *index,
                                    char *dest, size_t dest_size, char *error,
                                    size_t error_size) {
    if (!parser_is_token(tokens, token_count, *index, TOKEN_IDENTIFIER, NULL)) {
        utils_set_error(error, error_size, "Expected identifier.");
        return FAILURE;
    }

    if (utils_safe_strcpy(dest, dest_size, tokens[*index].value) != SUCCESS) {
        utils_set_error(error, error_size, "Identifier is too long.");
        return FAILURE;
    }

    (*index)++;
    return SUCCESS;
}

static int parser_expect_literal(const Token *tokens, int token_count, int *index,
                                 char *dest, size_t dest_size, char *error,
                                 size_t error_size) {
    TokenType type;

    if (tokens == NULL || index == NULL || dest == NULL) {
        return FAILURE;
    }

    if (*index >= token_count) {
        utils_set_error(error, error_size, "Expected literal value.");
        return FAILURE;
    }

    type = tokens[*index].type;
    if (type != TOKEN_INT_LITERAL && type != TOKEN_STR_LITERAL) {
        utils_set_error(error, error_size, "Expected literal value.");
        return FAILURE;
    }

    if (utils_safe_strcpy(dest, dest_size, tokens[*index].value) != SUCCESS) {
        utils_set_error(error, error_size, "Literal value is too long.");
        return FAILURE;
    }

    (*index)++;
    return SUCCESS;
}

static int parser_consume_optional_semicolon(const Token *tokens, int token_count,
                                             int *index, char *error,
                                             size_t error_size) {
    if (parser_is_token(tokens, token_count, *index, TOKEN_SEMICOLON, ";")) {
        (*index)++;
    }

    if (*index != token_count) {
        utils_set_error(error, error_size, "Unexpected trailing tokens.");
        return FAILURE;
    }

    return SUCCESS;
}

static int parser_parse_insert(const Token *tokens, int token_count,
                               SqlStatement *out, char *error,
                               size_t error_size) {
    int index;
    int value_count;

    index = 0;
    value_count = 0;
    memset(out, 0, sizeof(*out));
    out->type = SQL_INSERT;

    if (parser_expect_keyword(tokens, token_count, &index, "INSERT", error,
                              error_size) != SUCCESS ||
        parser_expect_keyword(tokens, token_count, &index, "INTO", error,
                              error_size) != SUCCESS) {
        return FAILURE;
    }

    if (parser_expect_identifier(tokens, token_count, &index,
                                 out->insert.table_name,
                                 sizeof(out->insert.table_name),
                                 error, error_size) != SUCCESS) {
        return FAILURE;
    }

    if (!parser_is_token(tokens, token_count, index, TOKEN_LPAREN, "(")) {
        utils_set_error(error, error_size, "Expected '(' after table name.");
        return FAILURE;
    }
    index++;

    while (index < token_count) {
        if (out->insert.column_count >= MAX_COLUMNS) {
            utils_set_error(error, error_size, "Too many columns in INSERT statement.");
            return FAILURE;
        }

        if (parser_expect_identifier(tokens, token_count, &index,
                                     out->insert.columns[out->insert.column_count],
                                     sizeof(out->insert.columns[0]),
                                     error, error_size) != SUCCESS) {
            return FAILURE;
        }
        out->insert.column_count++;

        if (parser_is_token(tokens, token_count, index, TOKEN_COMMA, ",")) {
            index++;
            continue;
        }
        break;
    }

    if (!parser_is_token(tokens, token_count, index, TOKEN_RPAREN, ")")) {
        utils_set_error(error, error_size, "Expected ')' after column list.");
        return FAILURE;
    }
    index++;

    if (parser_expect_keyword(tokens, token_count, &index, "VALUES", error,
                              error_size) != SUCCESS) {
        return FAILURE;
    }

    if (!parser_is_token(tokens, token_count, index, TOKEN_LPAREN, "(")) {
        utils_set_error(error, error_size, "Expected '(' before VALUES list.");
        return FAILURE;
    }
    index++;

    while (index < token_count) {
        if (value_count >= MAX_COLUMNS) {
            utils_set_error(error, error_size, "Too many values in INSERT statement.");
            return FAILURE;
        }

        if (parser_expect_literal(tokens, token_count, &index,
                                  out->insert.values[value_count],
                                  sizeof(out->insert.values[0]),
                                  error, error_size) != SUCCESS) {
            return FAILURE;
        }
        value_count++;

        if (parser_is_token(tokens, token_count, index, TOKEN_COMMA, ",")) {
            index++;
            continue;
        }
        break;
    }

    if (!parser_is_token(tokens, token_count, index, TOKEN_RPAREN, ")")) {
        utils_set_error(error, error_size, "Expected ')' after VALUES list.");
        return FAILURE;
    }
    index++;

    if (out->insert.column_count != value_count) {
        utils_set_error(error, error_size, "Column count doesn't match value count.");
        return FAILURE;
    }

    return parser_consume_optional_semicolon(tokens, token_count, &index,
                                             error, error_size);
}

static int parser_parse_select_columns(const Token *tokens, int token_count,
                                       int *index, SelectStatement *stmt,
                                       char *error, size_t error_size) {
    if (parser_is_token(tokens, token_count, *index, TOKEN_IDENTIFIER, "*")) {
        stmt->column_count = 0;
        (*index)++;
        return SUCCESS;
    }

    while (*index < token_count) {
        if (stmt->column_count >= MAX_COLUMNS) {
            utils_set_error(error, error_size, "Too many columns in SELECT statement.");
            return FAILURE;
        }

        if (parser_expect_identifier(tokens, token_count, index,
                                     stmt->columns[stmt->column_count],
                                     sizeof(stmt->columns[0]),
                                     error, error_size) != SUCCESS) {
            return FAILURE;
        }
        stmt->column_count++;

        if (parser_is_token(tokens, token_count, *index, TOKEN_COMMA, ",")) {
            (*index)++;
            continue;
        }
        break;
    }

    return SUCCESS;
}

static int parser_parse_where(const Token *tokens, int token_count, int *index,
                              WhereClause *where, char *error,
                              size_t error_size) {
    if (parser_expect_identifier(tokens, token_count, index,
                                 where->column, sizeof(where->column),
                                 error, error_size) != SUCCESS) {
        return FAILURE;
    }

    if (!parser_is_token(tokens, token_count, *index, TOKEN_OPERATOR, NULL)) {
        utils_set_error(error, error_size, "Expected operator in WHERE clause.");
        return FAILURE;
    }

    if (utils_safe_strcpy(where->op, sizeof(where->op),
                          tokens[*index].value) != SUCCESS) {
        utils_set_error(error, error_size, "WHERE operator is invalid.");
        return FAILURE;
    }
    (*index)++;

    if (parser_expect_literal(tokens, token_count, index,
                              where->value, sizeof(where->value),
                              error, error_size) != SUCCESS) {
        return FAILURE;
    }

    return SUCCESS;
}

static int parser_parse_select(const Token *tokens, int token_count,
                               SqlStatement *out, char *error,
                               size_t error_size) {
    int index;

    index = 0;
    memset(out, 0, sizeof(*out));
    out->type = SQL_SELECT;

    if (parser_expect_keyword(tokens, token_count, &index, "SELECT", error,
                              error_size) != SUCCESS) {
        return FAILURE;
    }

    if (parser_parse_select_columns(tokens, token_count, &index,
                                    &out->select, error, error_size) != SUCCESS) {
        return FAILURE;
    }

    if (parser_expect_keyword(tokens, token_count, &index, "FROM", error,
                              error_size) != SUCCESS) {
        return FAILURE;
    }

    if (parser_expect_identifier(tokens, token_count, &index,
                                 out->select.table_name,
                                 sizeof(out->select.table_name),
                                 error, error_size) != SUCCESS) {
        return FAILURE;
    }

    if (parser_is_token(tokens, token_count, index, TOKEN_KEYWORD, "WHERE")) {
        out->select.has_where = 1;
        index++;
        if (parser_parse_where(tokens, token_count, &index,
                               &out->select.where,
                               error, error_size) != SUCCESS) {
            return FAILURE;
        }
    }

    return parser_consume_optional_semicolon(tokens, token_count, &index,
                                             error, error_size);
}

static int parser_parse_delete(const Token *tokens, int token_count,
                               SqlStatement *out, char *error,
                               size_t error_size) {
    int index;

    index = 0;
    memset(out, 0, sizeof(*out));
    out->type = SQL_DELETE;

    if (parser_expect_keyword(tokens, token_count, &index, "DELETE", error,
                              error_size) != SUCCESS ||
        parser_expect_keyword(tokens, token_count, &index, "FROM", error,
                              error_size) != SUCCESS) {
        return FAILURE;
    }

    if (parser_expect_identifier(tokens, token_count, &index,
                                 out->delete_stmt.table_name,
                                 sizeof(out->delete_stmt.table_name),
                                 error, error_size) != SUCCESS) {
        return FAILURE;
    }

    if (parser_is_token(tokens, token_count, index, TOKEN_KEYWORD, "WHERE")) {
        out->delete_stmt.has_where = 1;
        index++;
        if (parser_parse_where(tokens, token_count, &index,
                               &out->delete_stmt.where,
                               error, error_size) != SUCCESS) {
            return FAILURE;
        }
    }

    return parser_consume_optional_semicolon(tokens, token_count, &index,
                                             error, error_size);
}

int parser_parse_with_error(const Token *tokens, int token_count, SqlStatement *out,
                            char *error, size_t error_size) {
    if (tokens == NULL || token_count <= 0 || out == NULL) {
        utils_set_error(error, error_size, "Empty SQL statement.");
        return FAILURE;
    }

    utils_set_error(error, error_size, "");

    if (parser_is_token(tokens, token_count, 0, TOKEN_KEYWORD, "INSERT")) {
        return parser_parse_insert(tokens, token_count, out, error, error_size);
    }

    if (parser_is_token(tokens, token_count, 0, TOKEN_KEYWORD, "SELECT")) {
        return parser_parse_select(tokens, token_count, out, error, error_size);
    }

    if (parser_is_token(tokens, token_count, 0, TOKEN_KEYWORD, "DELETE")) {
        return parser_parse_delete(tokens, token_count, out, error, error_size);
    }

    utils_set_error(error, error_size, "Unsupported SQL statement.");
    return FAILURE;
}

int parser_parse(const Token *tokens, int token_count, SqlStatement *out) {
    return parser_parse_with_error(tokens, token_count, out, NULL, 0);
}
