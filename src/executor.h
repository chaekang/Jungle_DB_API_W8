#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "parser.h"
#include "query_result.h"
#include "table_runtime.h"

int executor_execute_with_runtime(const SqlStatement *statement,
                                  TableRuntime *table,
                                  QueryResult *result);

#endif
