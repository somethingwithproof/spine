#ifndef SPINE_DB_CONNECTION_H
#define SPINE_DB_CONNECTION_H

#include "common.h"
#include "spine.h"

void spine_db_connection_connect(int type, MYSQL *mysql);
void spine_db_connection_disconnect(MYSQL *mysql);
void spine_db_connection_pool_create(int type);
void spine_db_connection_pool_close(int type);
pool_t *spine_db_connection_get(int type);
void spine_db_connection_release(int type, int id);

#endif
