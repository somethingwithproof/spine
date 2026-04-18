#include "common.h"
#include "spine.h"
#include "db_retry.h"

int spine_db_retry_reconnect(MYSQL *mysql, int type, int error, const char *function) {
	return db_reconnect(mysql, type, error, function);
}
