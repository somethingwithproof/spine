#include "common.h"
#include "spine.h"
#include "db_connection.h"

void spine_db_connection_connect(int type, MYSQL *mysql) {
	db_connect(type, mysql);
}

void spine_db_connection_disconnect(MYSQL *mysql) {
	db_disconnect(mysql);
}

void spine_db_connection_pool_create(int type) {
	db_create_connection_pool(type);
}

void spine_db_connection_pool_close(int type) {
	db_close_connection_pool(type);
}

pool_t *spine_db_connection_get(int type) {
	return db_get_connection(type);
}

void spine_db_connection_release(int type, int id) {
	db_release_connection(type, id);
}
