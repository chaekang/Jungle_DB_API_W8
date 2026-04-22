#ifndef QUERY_RESULT_H
#define QUERY_RESULT_H

#include "utils.h"

typedef enum {
    QUERY_RESULT_MESSAGE,
    QUERY_RESULT_TABLE
} QueryResultKind;

typedef struct {
    int success;
    QueryResultKind kind;
    char message[256];
    char error[256];
    char columns[MAX_COLUMNS][MAX_IDENTIFIER_LEN];
    int column_count;
    char ***rows;
    int row_count;
} QueryResult;

void query_result_init(QueryResult *result);
void query_result_free(QueryResult *result);
void query_result_print(FILE *stream, const QueryResult *result);

#endif
