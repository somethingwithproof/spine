#ifndef SPINE_DB_QUERY_H
#define SPINE_DB_QUERY_H

#include "common.h"
#include "spine.h"

MYSQL_RES *spine_db_query_exec(MYSQL *mysql, int type, const char *query);
int spine_db_query_insert(MYSQL *mysql, int type, const char *query);
void spine_db_query_escape(MYSQL *mysql, char *to, int to_len, const char *from);

#endif
