#include "common.h"
#include "spine.h"
#include "db_query.h"

MYSQL_RES *spine_db_query_exec(MYSQL *mysql, int type, const char *query) {
	return db_query(mysql, type, query);
}

int spine_db_query_insert(MYSQL *mysql, int type, const char *query) {
	return db_insert(mysql, type, query);
}

void spine_db_query_escape(MYSQL *mysql, char *to, int to_len, const char *from) {
	db_escape(mysql, to, to_len, from);
}
