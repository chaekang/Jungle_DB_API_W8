#ifndef ENGINE_H
#define ENGINE_H

#include "query_result.h"

int engine_execute_sql(const char *sql, QueryResult *out_result);

#endif
