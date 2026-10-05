/*
 ex: set tabstop=4 shiftwidth=4 autoindent:
 +-------------------------------------------------------------------------+
 | Copyright (C) 2004-2026 The Cacti Group                                 |
 +-------------------------------------------------------------------------+
 | db_escape regression tests ported from fix/escape-sizing.               |
 +-------------------------------------------------------------------------+
*/

#include "common.h"
#include "spine.h"
#include "sql.h"
#include "test_platform_helpers.h"

int spine_log(const char *format, ...) {
	(void) format;
	return 0;
}

void die(const char *format, ...) {
	(void) format;
	exit(1);
}

static void test_metacharacters(MYSQL *mysql) {
	char out[64];
	db_escape(mysql, out, sizeof out, "a'b");
	ASSERT_TRUE(strcmp(out, "a\\'b") == 0);
	db_escape(mysql, out, sizeof out, "back\\slash");
	ASSERT_TRUE(strcmp(out, "back\\\\slash") == 0);
	db_escape(mysql, out, sizeof out, "plain value");
	ASSERT_TRUE(strcmp(out, "plain value") == 0);
}

static void test_null_input(MYSQL *mysql) {
	char out[16] = "untouched";
	db_escape(mysql, out, sizeof out, NULL);
	ASSERT_TRUE(strcmp(out, "untouched") == 0);
	db_escape(mysql, NULL, 16, "anything");
}

static void test_full_result(MYSQL *mysql) {
	char input[RESULTS_BUFFER];
	char out[(RESULTS_BUFFER * 2) + 1];
	memset(input, 'x', sizeof input - 1);
	input[sizeof input - 1] = '\0';
	db_escape(mysql, out, sizeof out, input);
	ASSERT_INT_EQ(strlen(out), sizeof input - 1);
	ASSERT_TRUE(strcmp(out, input) == 0);

	/* Exercise the maximum expansion as well as a plain multi-value result. */
	memset(input, '\'', sizeof input - 1);
	db_escape(mysql, out, sizeof out, input);
	ASSERT_INT_EQ(strlen(out), 2 * (sizeof input - 1));
	for (size_t i = 0; i < sizeof input - 1; i++) {
		ASSERT_TRUE(out[2 * i] == '\\' && out[2 * i + 1] == '\'');
	}
}

static void test_staging_boundary(MYSQL *mysql) {
	const size_t sizes[] = {1022, 1023, 1024, 1100, 2047};
	char input[2048];
	char out[(2048 * 2) + 1];
	for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
		memset(input, 'y', sizes[i]);
		input[sizes[i]] = '\0';
		db_escape(mysql, out, sizeof out, input);
		ASSERT_INT_EQ(strlen(out), sizes[i]);
		ASSERT_TRUE(strcmp(out, input) == 0);
	}
}

static void test_small_destination(MYSQL *mysql) {
	char out[11];
	db_escape(mysql, out, sizeof out, "0123456789abcdef");
	ASSERT_TRUE(strcmp(out, "01234") == 0);
	/* Guard bytes prove that worst-case expansion stays inside the destination. */
	unsigned char guarded[13];
	memset(guarded, 0xa5, sizeof guarded);
	db_escape(mysql, (char *) guarded + 1, 11, "''''''''");
	ASSERT_TRUE(guarded[0] == 0xa5 && guarded[12] == 0xa5);
	ASSERT_TRUE(strcmp((char *) guarded + 1, "\\'\\'\\'\\'\\'") == 0);
}

static void test_degenerate_destination(MYSQL *mysql) {
	char out[4] = "abc";
	db_escape(mysql, out, 1, "anything");
	ASSERT_TRUE(out[0] == '\0' && out[1] == 'b');
	memcpy(out, "abc", 4);
	db_escape(mysql, out, 0, "anything");
	ASSERT_TRUE(strcmp(out, "abc") == 0);
	db_escape(mysql, out, -1, "anything");
	ASSERT_TRUE(strcmp(out, "abc") == 0);
	db_escape(mysql, out, 2, "'");
	ASSERT_TRUE(out[0] == '\0' && out[1] == 'b');
	db_escape(mysql, out, 3, "''");
	ASSERT_TRUE(strcmp(out, "\\'") == 0 && out[3] == '\0');
}

int main(void) {
	MYSQL *mysql = mysql_init(NULL);
	if (mysql == NULL) {
		ASSERT_FAIL("mysql_init failed");
		return finish_tests("db_escape tests");
	}
	test_metacharacters(mysql);
	test_null_input(mysql);
	test_full_result(mysql);
	test_staging_boundary(mysql);
	test_small_destination(mysql);
	test_degenerate_destination(mysql);
	mysql_close(mysql);
	return finish_tests("db_escape tests");
}
