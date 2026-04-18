#ifndef SPINE_DB_RETRY_H
#define SPINE_DB_RETRY_H

#include "common.h"
#include "spine.h"

int spine_db_retry_reconnect(MYSQL *mysql, int type, int error, const char *function);

#endif
