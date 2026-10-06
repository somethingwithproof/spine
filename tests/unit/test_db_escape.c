/*
 ex: set tabstop=4 shiftwidth=4 autoindent:
 +-------------------------------------------------------------------------+
 | Copyright (C) 2004-2026 The Cacti Group                                 |
 |                                                                         |
 | This program is free software; you can redistribute it and/or           |
 | modify it under the terms of the GNU Lesser General Public              |
 | License as published by the Free Software Foundation; either            |
 | version 2.1 of the License, or (at your option) any later version.      |
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
 | db_escape regression tests ported from fix/escape-sizing.               |
 +-------------------------------------------------------------------------+
*/

#include "common.h"
#include "spine.h"
#include "sql.h"
#include "output_buffer.h"
#include "test_platform_helpers.h"
#include <stdint.h>

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

static void test_output_query_assembly(MYSQL *mysql, const char *prefix) {
	const char suffix[] = " ON DUPLICATE KEY UPDATE output=VALUES(output)";
	char input[RESULTS_BUFFER];
	char escaped[(RESULTS_BUFFER * 2) + 1];
	char name[sizeof(((target_t *)0)->rrd_name)];
	char escaped_name[DBL_BUFSIZE];
	char tuple[(RESULTS_BUFFER * 2) + DBL_BUFSIZE + SMALL_BUFSIZE];
	const size_t capacity = spine_output_buffer_size(MAX_MYSQL_BUF_SIZE,
		sizeof(tuple), strlen(prefix), strlen(suffix));
	ASSERT_TRUE(capacity > 0 && capacity < SIZE_MAX - 2);
	if (capacity == 0 || capacity >= SIZE_MAX - 2) return;
	unsigned char *guarded = malloc(capacity + 2);
	ASSERT_TRUE(guarded != NULL);
	if (guarded == NULL) return;
	char *query = (char *)guarded + 1;
	memset(input, '\'', sizeof(input) - 1);
	input[sizeof(input) - 1] = '\0';
	db_escape(mysql, escaped, sizeof(escaped), input);
	ASSERT_TRUE(strlen(escaped) == 2 * (sizeof(input) - 1));
	memset(name, '\'', sizeof(name) - 1);
	name[sizeof(name) - 1] = '\0';
	db_escape(mysql, escaped_name, sizeof(escaped_name), name);
	ASSERT_TRUE(strlen(escaped_name) == 2 * (sizeof(name) - 1));

	/* Exercise two consecutive maximum-expansion rows. The real writers flush at the
	 * shared threshold before appending each row and reset to this prefix.
	 * Test the actual assembly primitives, not a fake mysql writer. */
	for (int row = 1; row <= 2; row++) {
		memset(guarded, 0xa5, capacity + 2);
		query[0] = '\0';
		const int prefix_appended = spine_output_buffer_append(query, capacity, prefix, strlen(prefix));
		ASSERT_TRUE(prefix_appended);
		if (!prefix_appended) break;
		const int formatted = snprintf(tuple, sizeof(tuple),
			" (%i, '%s', FROM_UNIXTIME(1700000000), '%s')", row, escaped_name, escaped);
		ASSERT_TRUE(formatted >= 0 && (size_t)formatted < sizeof(tuple));
		if (formatted < 0 || (size_t)formatted >= sizeof(tuple)) break;
		const size_t length = (size_t)formatted;
		if (length >= MAX_MYSQL_BUF_SIZE) {
			ASSERT_TRUE(spine_output_buffer_needs_flush(strlen(prefix), length, MAX_MYSQL_BUF_SIZE));
		}
		const int tuple_appended = spine_output_buffer_append(query, capacity, tuple, length);
		ASSERT_TRUE(tuple_appended);
		if (!tuple_appended) break;
		const int suffix_appended = spine_output_buffer_append(query, capacity, suffix, strlen(suffix));
		ASSERT_TRUE(suffix_appended);
		if (!suffix_appended) break;
		ASSERT_TRUE(strlen(query) == strlen(prefix) + length + strlen(suffix));
		ASSERT_TRUE(memcmp(query, prefix, strlen(prefix)) == 0);
		ASSERT_TRUE(memcmp(query + strlen(prefix), tuple, length) == 0);
		ASSERT_TRUE(strcmp(query + strlen(prefix) + length, suffix) == 0);
		ASSERT_TRUE(guarded[0] == 0xa5 && guarded[capacity + 1] == 0xa5);
	}
	free(guarded);
}

static void test_output_buffer_boundaries(void) {
	char buffer[8] = "prefix";
	ASSERT_TRUE(!spine_output_buffer_append(buffer, sizeof(buffer), "ab", 2));
	ASSERT_TRUE(strcmp(buffer, "prefix") == 0);
	ASSERT_TRUE(spine_output_buffer_append(buffer, sizeof(buffer), "x", 1));
	ASSERT_TRUE(strcmp(buffer, "prefixx") == 0);
	ASSERT_TRUE(!spine_output_buffer_append(buffer, sizeof(buffer), "x", 1));
	memset(buffer, 'x', sizeof(buffer));
	ASSERT_TRUE(!spine_output_buffer_append(buffer, sizeof(buffer), "y", 1));
	for (size_t i = 0; i < sizeof(buffer); i++) ASSERT_TRUE(buffer[i] == 'x');
	ASSERT_TRUE(!spine_output_buffer_append(NULL, 8, "x", 1));
	ASSERT_TRUE(!spine_output_buffer_append(buffer, 0, "x", 1));
	ASSERT_TRUE(!spine_output_buffer_append(buffer, sizeof(buffer), NULL, 0));
	ASSERT_TRUE(spine_output_buffer_size(SIZE_MAX, 1, 1, 1) == 0);
	ASSERT_TRUE(spine_output_buffer_size(1, SIZE_MAX, 1, 1) == 0);
	ASSERT_TRUE(spine_output_buffer_size(1, 1, SIZE_MAX, 1) == 0);
	ASSERT_TRUE(spine_output_buffer_size(1, 1, 1, SIZE_MAX) == 0);
	ASSERT_TRUE(!spine_output_buffer_needs_flush(10, 20, 31));
	ASSERT_TRUE(spine_output_buffer_needs_flush(10, 20, 30));
	ASSERT_TRUE(spine_output_buffer_needs_flush(SIZE_MAX, 1, SIZE_MAX));
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
	test_output_query_assembly(mysql,
		"INSERT INTO poller_output (local_data_id, rrd_name, time, output) VALUES");
	test_output_query_assembly(mysql,
		"INSERT INTO poller_output_boost (local_data_id, rrd_name, time, output) VALUES");
	test_output_buffer_boundaries();
	mysql_close(mysql);
	return finish_tests("db_escape tests");
}
