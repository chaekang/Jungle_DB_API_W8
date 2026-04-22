#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "parser.h"
#include "query_result.h"

/*
 * 파싱이 끝난 SQL 문 하나를 구조화된 결과로 실행한다.
 * 성공 시 SUCCESS, 실패 시 FAILURE를 반환한다.
 */
int executor_execute_to_result(const SqlStatement *statement, QueryResult *result);

/*
 * 기존 CLI 경로 호환용 함수다.
 * 결과를 stdout/stderr 성격으로 출력하고 성공 여부를 반환한다.
 */
int executor_execute(const SqlStatement *statement);

#endif
