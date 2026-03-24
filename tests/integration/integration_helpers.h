/*
 ex: set tabstop=4 shiftwidth=4 autoindent:
 +-------------------------------------------------------------------------+
 | Copyright (C) 2004-2026 The Cacti Group                                 |
 |                                                                         |
 | This program is free software; you can redistribute it and/or           |
 | modify it under the terms of the GNU Lesser General Public              |
 | License as published by the Free Software Foundation; either            |
 | version 2.1 of the License, or (at your option) any later version.     |
 |                                                                         |
 | This program is distributed in the hope that it will be useful,         |
 | but WITHOUT ANY WARRANTY; without even the implied warranty of          |
 | MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           |
 | GNU Lesser General Public License for more details.                     |
 |                                                                         |
 | You should have received a copy of the GNU Lesser General Public        |
 | License along with this library; if not, write to the Free Software     |
 | Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA           |
 | 02110-1301, USA                                                         |
 |                                                                         |
 +-------------------------------------------------------------------------+
 | spine: a backend data gatherer for cacti                                |
 +-------------------------------------------------------------------------+
 | Integration test helpers: DB setup, teardown, fixtures                  |
 +-------------------------------------------------------------------------+
*/

#ifndef SPINE_INTEGRATION_HELPERS_H
#define SPINE_INTEGRATION_HELPERS_H

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <mysql.h>

/*
 * Environment variable names for DB credentials.
 * Falls back to localhost/root/empty/cacti_test if unset.
 */
#define TEST_DB_HOST_ENV "DB_HOST"
#define TEST_DB_USER_ENV "DB_USER"
#define TEST_DB_PASS_ENV "DB_PASS"
#define TEST_DB_NAME_ENV "DB_NAME"
#define TEST_DB_PORT_ENV "DB_PORT"

#define TEST_DB_HOST_DEFAULT "127.0.0.1"
#define TEST_DB_USER_DEFAULT "root"
#define TEST_DB_PASS_DEFAULT ""
#define TEST_DB_NAME_DEFAULT "cacti_test"
#define TEST_DB_PORT_DEFAULT 3306

/*! \fn static const char *test_env_or_default(const char *env, const char *dflt)
 *  \brief returns environment variable value or a default
 */
static const char *test_env_or_default(const char *env, const char *dflt) {
	const char *val = getenv(env);
	if (val != NULL && val[0] != '\0') {
		return val;
	}
	return dflt;
}

/*! \fn static int test_env_port(void)
 *  \brief returns DB port from environment or default
 */
static int test_env_port(void) {
	const char *val = getenv(TEST_DB_PORT_ENV);
	if (val != NULL && val[0] != '\0') {
		return atoi(val);
	}
	return TEST_DB_PORT_DEFAULT;
}

/*! \fn static MYSQL *test_db_connect(void)
 *  \brief opens a MySQL connection using test environment credentials
 *  \return a connected MYSQL handle, or NULL on failure
 */
static MYSQL *test_db_connect(void) {
	MYSQL *mysql;
	const char *host;
	const char *user;
	const char *pass;
	const char *db;
	int port;

	host = test_env_or_default(TEST_DB_HOST_ENV, TEST_DB_HOST_DEFAULT);
	user = test_env_or_default(TEST_DB_USER_ENV, TEST_DB_USER_DEFAULT);
	pass = test_env_or_default(TEST_DB_PASS_ENV, TEST_DB_PASS_DEFAULT);
	db   = test_env_or_default(TEST_DB_NAME_ENV, TEST_DB_NAME_DEFAULT);
	port = test_env_port();

	mysql = mysql_init(NULL);
	if (mysql == NULL) {
		fprintf(stderr, "TEST: mysql_init() failed\n");
		return NULL;
	}

	if (!mysql_real_connect(mysql, host, user, pass, db, port, NULL, 0)) {
		fprintf(stderr, "TEST: mysql_real_connect() failed: %s\n",
			mysql_error(mysql));
		mysql_close(mysql);
		return NULL;
	}

	return mysql;
}

/*! \fn static void test_db_disconnect(MYSQL *mysql)
 *  \brief closes a test MySQL connection
 */
static void test_db_disconnect(MYSQL *mysql) {
	if (mysql != NULL) {
		mysql_close(mysql);
	}
}

/*! \fn static int test_db_exec(MYSQL *mysql, const char *query)
 *  \brief executes a SQL statement, returns 0 on success
 */
static int test_db_exec(MYSQL *mysql, const char *query) {
	if (mysql_query(mysql, query)) {
		fprintf(stderr, "TEST SQL error: %s\nQuery: %s\n",
			mysql_error(mysql), query);
		return -1;
	}
	return 0;
}

/*! \fn static int test_create_schema(MYSQL *mysql)
 *  \brief creates the minimum test schema required by spine integration tests
 *  \return 0 on success, -1 on failure
 */
static int test_create_schema(MYSQL *mysql) {
	/* settings table used by read_config_options / getsetting */
	if (test_db_exec(mysql,
		"CREATE TABLE IF NOT EXISTS settings ("
		"  name VARCHAR(255) NOT NULL DEFAULT '',"
		"  value VARCHAR(1024) NOT NULL DEFAULT '',"
		"  PRIMARY KEY (name)"
		") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4")) {
		return -1;
	}

	/* poller table used by getpsetting */
	if (test_db_exec(mysql,
		"CREATE TABLE IF NOT EXISTS poller ("
		"  id INT UNSIGNED NOT NULL AUTO_INCREMENT,"
		"  name VARCHAR(255) NOT NULL DEFAULT '',"
		"  threads INT UNSIGNED NOT NULL DEFAULT 1,"
		"  dbdefault VARCHAR(255) NOT NULL DEFAULT '',"
		"  PRIMARY KEY (id)"
		") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4")) {
		return -1;
	}

	/* host table used by poller pipeline */
	if (test_db_exec(mysql,
		"CREATE TABLE IF NOT EXISTS host ("
		"  id INT UNSIGNED NOT NULL AUTO_INCREMENT,"
		"  hostname VARCHAR(250) NOT NULL DEFAULT '',"
		"  snmp_community VARCHAR(100) NOT NULL DEFAULT '',"
		"  snmp_version TINYINT UNSIGNED NOT NULL DEFAULT 0,"
		"  snmp_username VARCHAR(50) NOT NULL DEFAULT '',"
		"  snmp_password VARCHAR(50) NOT NULL DEFAULT '',"
		"  snmp_auth_protocol VARCHAR(7) NOT NULL DEFAULT '',"
		"  snmp_priv_passphrase VARCHAR(200) NOT NULL DEFAULT '',"
		"  snmp_priv_protocol VARCHAR(8) NOT NULL DEFAULT '',"
		"  snmp_context VARCHAR(65) NOT NULL DEFAULT '',"
		"  snmp_engine_id VARCHAR(30) NOT NULL DEFAULT '',"
		"  snmp_port INT UNSIGNED NOT NULL DEFAULT 161,"
		"  snmp_timeout INT UNSIGNED NOT NULL DEFAULT 500,"
		"  max_oids INT UNSIGNED NOT NULL DEFAULT 10,"
		"  availability_method SMALLINT UNSIGNED NOT NULL DEFAULT 1,"
		"  ping_method SMALLINT UNSIGNED NOT NULL DEFAULT 0,"
		"  ping_port INT UNSIGNED NOT NULL DEFAULT 23,"
		"  ping_timeout INT UNSIGNED NOT NULL DEFAULT 400,"
		"  ping_retries INT UNSIGNED NOT NULL DEFAULT 1,"
		"  status TINYINT NOT NULL DEFAULT 0,"
		"  disabled CHAR(2) NOT NULL DEFAULT '',"
		"  status_event_count INT UNSIGNED NOT NULL DEFAULT 0,"
		"  status_fail_date VARCHAR(40) NOT NULL DEFAULT '0000-00-00 00:00:00',"
		"  status_rec_date VARCHAR(40) NOT NULL DEFAULT '0000-00-00 00:00:00',"
		"  status_last_error VARCHAR(255) NOT NULL DEFAULT '',"
		"  min_time DOUBLE NOT NULL DEFAULT 9.99999,"
		"  max_time DOUBLE NOT NULL DEFAULT 0,"
		"  cur_time DOUBLE NOT NULL DEFAULT 0,"
		"  avg_time DOUBLE NOT NULL DEFAULT 0,"
		"  total_polls INT UNSIGNED NOT NULL DEFAULT 0,"
		"  failed_polls INT UNSIGNED NOT NULL DEFAULT 0,"
		"  availability DOUBLE NOT NULL DEFAULT 100,"
		"  poller_id INT UNSIGNED NOT NULL DEFAULT 1,"
		"  PRIMARY KEY (id)"
		") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4")) {
		return -1;
	}

	/* poller_item table used by poller pipeline */
	if (test_db_exec(mysql,
		"CREATE TABLE IF NOT EXISTS poller_item ("
		"  local_data_id INT UNSIGNED NOT NULL DEFAULT 0,"
		"  poller_id INT UNSIGNED NOT NULL DEFAULT 1,"
		"  host_id INT UNSIGNED NOT NULL DEFAULT 0,"
		"  action TINYINT UNSIGNED NOT NULL DEFAULT 0,"
		"  hostname VARCHAR(250) NOT NULL DEFAULT '',"
		"  snmp_community VARCHAR(100) NOT NULL DEFAULT '',"
		"  snmp_version TINYINT UNSIGNED NOT NULL DEFAULT 0,"
		"  snmp_username VARCHAR(50) NOT NULL DEFAULT '',"
		"  snmp_password VARCHAR(50) NOT NULL DEFAULT '',"
		"  snmp_auth_protocol VARCHAR(7) NOT NULL DEFAULT '',"
		"  snmp_priv_passphrase VARCHAR(200) NOT NULL DEFAULT '',"
		"  snmp_priv_protocol VARCHAR(8) NOT NULL DEFAULT '',"
		"  snmp_context VARCHAR(65) NOT NULL DEFAULT '',"
		"  snmp_engine_id VARCHAR(30) NOT NULL DEFAULT '',"
		"  snmp_port INT UNSIGNED NOT NULL DEFAULT 161,"
		"  snmp_timeout INT UNSIGNED NOT NULL DEFAULT 500,"
		"  rrd_name VARCHAR(30) NOT NULL DEFAULT '',"
		"  rrd_path VARCHAR(255) NOT NULL DEFAULT '',"
		"  rrd_num TINYINT UNSIGNED NOT NULL DEFAULT 0,"
		"  rrd_step INT NOT NULL DEFAULT 300,"
		"  arg1 TEXT NOT NULL,"
		"  arg2 VARCHAR(255) NOT NULL DEFAULT '',"
		"  arg3 VARCHAR(255) NOT NULL DEFAULT '',"
		"  PRIMARY KEY (local_data_id, poller_id)"
		") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4")) {
		return -1;
	}

	/* poller_output table for storing results */
	if (test_db_exec(mysql,
		"CREATE TABLE IF NOT EXISTS poller_output ("
		"  local_data_id INT UNSIGNED NOT NULL DEFAULT 0,"
		"  rrd_name VARCHAR(30) NOT NULL DEFAULT '',"
		"  time DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
		"  output VARCHAR(512) NOT NULL DEFAULT '',"
		"  PRIMARY KEY (local_data_id, rrd_name, time)"
		") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4")) {
		return -1;
	}

	return 0;
}

/*! \fn static int test_insert_fixtures(MYSQL *mysql)
 *  \brief inserts sample test data into the schema
 *  \return 0 on success, -1 on failure
 */
static int test_insert_fixtures(MYSQL *mysql) {
	/* sample settings */
	if (test_db_exec(mysql,
		"INSERT IGNORE INTO settings (name, value) VALUES"
		" ('log_verbosity', '5'),"
		" ('poller_interval', '300'),"
		" ('max_threads', '5'),"
		" ('spine_log_level', '3')")) {
		return -1;
	}

	/* sample poller */
	if (test_db_exec(mysql,
		"INSERT IGNORE INTO poller (id, name, threads) VALUES"
		" (1, 'Main Poller', 5)")) {
		return -1;
	}

	/* sample host */
	if (test_db_exec(mysql,
		"INSERT IGNORE INTO host (id, hostname, snmp_community, snmp_version,"
		"  availability_method, ping_method, status, poller_id) VALUES"
		" (1, '127.0.0.1', 'public', 2, 1, 2, 3, 1)")) {
		return -1;
	}

	/* sample poller_item */
	if (test_db_exec(mysql,
		"INSERT IGNORE INTO poller_item (local_data_id, poller_id, host_id,"
		"  action, hostname, snmp_community, snmp_version, snmp_port,"
		"  snmp_timeout, rrd_name, rrd_path, arg1) VALUES"
		" (1, 1, 1, 0, '127.0.0.1', 'public', 2, 161, 500,"
		"  'traffic_in', '/var/lib/cacti/rra/test.rrd',"
		"  '.1.3.6.1.2.1.2.2.1.10.1')")) {
		return -1;
	}

	return 0;
}

/*! \fn static void test_cleanup_tables(MYSQL *mysql)
 *  \brief truncates all test tables for idempotent runs
 */
static void test_cleanup_tables(MYSQL *mysql) {
	test_db_exec(mysql, "DELETE FROM poller_output");
	test_db_exec(mysql, "DELETE FROM poller_item");
	test_db_exec(mysql, "DELETE FROM host");
	test_db_exec(mysql, "DELETE FROM poller");
	test_db_exec(mysql, "DELETE FROM settings");
}

/*! \fn static void test_drop_tables(MYSQL *mysql)
 *  \brief drops all test tables
 */
static void test_drop_tables(MYSQL *mysql) {
	test_db_exec(mysql, "DROP TABLE IF EXISTS poller_output");
	test_db_exec(mysql, "DROP TABLE IF EXISTS poller_item");
	test_db_exec(mysql, "DROP TABLE IF EXISTS host");
	test_db_exec(mysql, "DROP TABLE IF EXISTS poller");
	test_db_exec(mysql, "DROP TABLE IF EXISTS settings");
}

#endif /* SPINE_INTEGRATION_HELPERS_H */
