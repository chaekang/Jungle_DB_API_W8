#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "parser.h"
#include "query_result.h"

/*
 * 파싱이 끝난 SQL 문 하나를 실행하고 구조화된 결과를 채운다.
 * 성공 시 SUCCESS, 실패 시 FAILURE를 반환한다.
 */
int executor_execute(const SqlStatement *statement, QueryResult *out_result);

#endif
