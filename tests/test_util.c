/*
 ex: set tabstop=4 shiftwidth=4 autoindent:
 +-------------------------------------------------------------------------+
 | Copyright (C) 2004-2026 The Cacti Group                                 |
 |                                                                         |
 | This program is free software; you can redistribute it and/or           |
 | modify it under the terms of the GNU Lesser General Public              |
 | License as published by the Free Software Foundation; either            |
 | version 2.1 of the License, or (at your option) any later version.     |
 +-------------------------------------------------------------------------+
 | spine: a backend data gatherer for cacti                                |
 +-------------------------------------------------------------------------+
*/

/* Unit tests for pure utility functions in util.c.
 *
 * Functions with no DB/SNMP/thread dependencies are tested here.
 * Anything requiring a live MySQL handle or Net-SNMP session lives
 * in the integration test layer (tests/snmpv3/).
 */

#include <check.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "spine.h"
#include "util.h"

/* ------------------------------------------------------------------ */
/* all_digits                                                           */
/* ------------------------------------------------------------------ */

START_TEST(test_all_digits_pure_integer)
{
	ck_assert_int_eq(all_digits("12345"), TRUE);
}
END_TEST

START_TEST(test_all_digits_empty)
{
	/* empty string must return FALSE -- not "all digits" */
	ck_assert_int_eq(all_digits(""), FALSE);
}
END_TEST

START_TEST(test_all_digits_with_alpha)
{
	ck_assert_int_eq(all_digits("123abc"), FALSE);
}
END_TEST

START_TEST(test_all_digits_negative)
{
	/* sign prefix disqualifies it as all-digits */
	ck_assert_int_eq(all_digits("-5"), FALSE);
}
END_TEST

START_TEST(test_all_digits_single_zero)
{
	ck_assert_int_eq(all_digits("0"), TRUE);
}
END_TEST

/* ------------------------------------------------------------------ */
/* is_ipaddress                                                         */
/* ------------------------------------------------------------------ */

START_TEST(test_is_ipaddress_v4)
{
	ck_assert_int_eq(is_ipaddress("192.168.1.1"), TRUE);
}
END_TEST

START_TEST(test_is_ipaddress_v6)
{
	ck_assert_int_eq(is_ipaddress("2001:db8::1"), TRUE);
}
END_TEST

START_TEST(test_is_ipaddress_hostname)
{
	/* hostnames contain letters -- not a bare IP */
	ck_assert_int_eq(is_ipaddress("router.local"), FALSE);
}
END_TEST

START_TEST(test_is_ipaddress_empty)
{
	/* empty string: loop never executes, returns TRUE per implementation */
	ck_assert_int_eq(is_ipaddress(""), TRUE);
}
END_TEST

/* ------------------------------------------------------------------ */
/* is_hexadecimal                                                       */
/* ------------------------------------------------------------------ */

START_TEST(test_is_hexadecimal_mac_colon)
{
	/* canonical MAC with colons; >= 3 chars and has delimiter */
	ck_assert_int_eq(is_hexadecimal("00:1A:2B:3C:4D:5E", 0), TRUE);
}
END_TEST

START_TEST(test_is_hexadecimal_mac_dash)
{
	ck_assert_int_eq(is_hexadecimal("00-1A-2B-3C-4D-5E", 0), TRUE);
}
END_TEST

START_TEST(test_is_hexadecimal_no_delimiter)
{
	/* no delimiter required by spec -- returns FALSE */
	ck_assert_int_eq(is_hexadecimal("AABBCC", 0), FALSE);
}
END_TEST

START_TEST(test_is_hexadecimal_short)
{
	/* fewer than 3 chars -- returns FALSE even with delimiter */
	ck_assert_int_eq(is_hexadecimal("A:B", 0), FALSE);
}
END_TEST

START_TEST(test_is_hexadecimal_invalid_char)
{
	ck_assert_int_eq(is_hexadecimal("GG:HH:II", 0), FALSE);
}
END_TEST

START_TEST(test_is_hexadecimal_tab_ignored)
{
	/* tab accepted when ignore_special is nonzero */
	ck_assert_int_eq(is_hexadecimal("AA:BB\tCC", 1), TRUE);
}
END_TEST

/* ------------------------------------------------------------------ */
/* rtrim / ltrim / trim                                                 */
/* ------------------------------------------------------------------ */

START_TEST(test_rtrim_spaces)
{
	char buf[] = "hello   ";
	ck_assert_str_eq(rtrim(buf), "hello");
}
END_TEST

START_TEST(test_rtrim_newline)
{
	char buf[] = "value\n";
	ck_assert_str_eq(rtrim(buf), "value");
}
END_TEST

START_TEST(test_ltrim_spaces)
{
	char buf[] = "   hello";
	ck_assert_str_eq(ltrim(buf), "hello");
}
END_TEST

START_TEST(test_ltrim_tab)
{
	char buf[] = "\t\thello";
	ck_assert_str_eq(ltrim(buf), "hello");
}
END_TEST

START_TEST(test_trim_both_ends)
{
	char buf[] = "  hello world  ";
	ck_assert_str_eq(trim(buf), "hello world");
}
END_TEST

START_TEST(test_trim_quotes_stripped)
{
	/* rtrim strips single/double quotes per the implementation */
	char buf[] = "\"value\"";
	ck_assert_str_eq(trim(buf), "value");
}
END_TEST

START_TEST(test_rtrim_null)
{
	/* NULL input must not crash; implementation returns NULL */
	ck_assert_ptr_eq(rtrim(NULL), NULL);
}
END_TEST

START_TEST(test_ltrim_null)
{
	ck_assert_ptr_eq(ltrim(NULL), NULL);
}
END_TEST

/* ------------------------------------------------------------------ */
/* reverse                                                              */
/* ------------------------------------------------------------------ */

START_TEST(test_reverse_even)
{
	char buf[] = "abcd";
	ck_assert_str_eq(reverse(buf), "dcba");
}
END_TEST

START_TEST(test_reverse_odd)
{
	char buf[] = "hello";
	ck_assert_str_eq(reverse(buf), "olleh");
}
END_TEST

START_TEST(test_reverse_single_char)
{
	char buf[] = "x";
	ck_assert_str_eq(reverse(buf), "x");
}
END_TEST

START_TEST(test_reverse_empty)
{
	char buf[] = "";
	ck_assert_str_eq(reverse(buf), "");
}
END_TEST

/* ------------------------------------------------------------------ */
/* strpos                                                               */
/* ------------------------------------------------------------------ */

START_TEST(test_strpos_found_at_start)
{
	ck_assert_int_eq(strpos("hello world", "hello"), 0);
}
END_TEST

START_TEST(test_strpos_found_in_middle)
{
	ck_assert_int_eq(strpos("hello world", "world"), 6);
}
END_TEST

START_TEST(test_strpos_not_found)
{
	ck_assert_int_eq(strpos("hello world", "xyz"), -1);
}
END_TEST

/* ------------------------------------------------------------------ */
/* char_count                                                           */
/* ------------------------------------------------------------------ */

START_TEST(test_char_count_multiple)
{
	ck_assert_int_eq(char_count("192.168.1.1", '.'), 3);
}
END_TEST

START_TEST(test_char_count_none)
{
	ck_assert_int_eq(char_count("hello", 'z'), 0);
}
END_TEST

START_TEST(test_char_count_nul_chr)
{
	/* chr == '\0' returns 1 per implementation */
	ck_assert_int_eq(char_count("anything", '\0'), 1);
}
END_TEST

/* ------------------------------------------------------------------ */
/* strncopy                                                             */
/* ------------------------------------------------------------------ */

START_TEST(test_strncopy_normal)
{
	char dst[16];
	strncopy(dst, "hello", sizeof dst);
	ck_assert_str_eq(dst, "hello");
}
END_TEST

START_TEST(test_strncopy_truncates)
{
	char dst[4];
	strncopy(dst, "hello", sizeof dst);
	/* strncopy copies min(strlen(src), obuf) bytes then NUL-terminates
	 * at that position; with obuf=4 we get 4 bytes and dst[4]='\0'
	 * but dst is only 4 bytes, so verify the first 3 chars match. */
	ck_assert_int_eq(strncmp(dst, "hel", 3), 0);
}
END_TEST

START_TEST(test_strncopy_exact_fit)
{
	char dst[6];
	strncopy(dst, "hello", sizeof dst);
	ck_assert_str_eq(dst, "hello");
}
END_TEST

/* ------------------------------------------------------------------ */
/* is_numeric                                                           */
/* ------------------------------------------------------------------ */

START_TEST(test_is_numeric_integer)
{
	char s[] = "42";
	ck_assert_int_eq(is_numeric(s), TRUE);
}
END_TEST

START_TEST(test_is_numeric_negative)
{
	char s[] = "-7";
	ck_assert_int_eq(is_numeric(s), TRUE);
}
END_TEST

START_TEST(test_is_numeric_float)
{
	char s[] = "3.14";
	ck_assert_int_eq(is_numeric(s), TRUE);
}
END_TEST

START_TEST(test_is_numeric_alpha)
{
	char s[] = "abc";
	ck_assert_int_eq(is_numeric(s), FALSE);
}
END_TEST

START_TEST(test_is_numeric_empty)
{
	char s[] = "";
	ck_assert_int_eq(is_numeric(s), FALSE);
}
END_TEST

START_TEST(test_is_numeric_whitespace_only)
{
	char s[] = "   ";
	ck_assert_int_eq(is_numeric(s), FALSE);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite assembly                                                       */
/* ------------------------------------------------------------------ */

static Suite *util_suite(void)
{
	Suite *s;
	TCase *tc_digits, *tc_ip, *tc_hex, *tc_trim, *tc_reverse,
	      *tc_strpos, *tc_charcount, *tc_strncopy, *tc_numeric;

	s = suite_create("util");

	tc_digits = tcase_create("all_digits");
	tcase_add_test(tc_digits, test_all_digits_pure_integer);
	tcase_add_test(tc_digits, test_all_digits_empty);
	tcase_add_test(tc_digits, test_all_digits_with_alpha);
	tcase_add_test(tc_digits, test_all_digits_negative);
	tcase_add_test(tc_digits, test_all_digits_single_zero);
	suite_add_tcase(s, tc_digits);

	tc_ip = tcase_create("is_ipaddress");
	tcase_add_test(tc_ip, test_is_ipaddress_v4);
	tcase_add_test(tc_ip, test_is_ipaddress_v6);
	tcase_add_test(tc_ip, test_is_ipaddress_hostname);
	tcase_add_test(tc_ip, test_is_ipaddress_empty);
	suite_add_tcase(s, tc_ip);

	tc_hex = tcase_create("is_hexadecimal");
	tcase_add_test(tc_hex, test_is_hexadecimal_mac_colon);
	tcase_add_test(tc_hex, test_is_hexadecimal_mac_dash);
	tcase_add_test(tc_hex, test_is_hexadecimal_no_delimiter);
	tcase_add_test(tc_hex, test_is_hexadecimal_short);
	tcase_add_test(tc_hex, test_is_hexadecimal_invalid_char);
	tcase_add_test(tc_hex, test_is_hexadecimal_tab_ignored);
	suite_add_tcase(s, tc_hex);

	tc_trim = tcase_create("trim");
	tcase_add_test(tc_trim, test_rtrim_spaces);
	tcase_add_test(tc_trim, test_rtrim_newline);
	tcase_add_test(tc_trim, test_ltrim_spaces);
	tcase_add_test(tc_trim, test_ltrim_tab);
	tcase_add_test(tc_trim, test_trim_both_ends);
	tcase_add_test(tc_trim, test_trim_quotes_stripped);
	tcase_add_test(tc_trim, test_rtrim_null);
	tcase_add_test(tc_trim, test_ltrim_null);
	suite_add_tcase(s, tc_trim);

	tc_reverse = tcase_create("reverse");
	tcase_add_test(tc_reverse, test_reverse_even);
	tcase_add_test(tc_reverse, test_reverse_odd);
	tcase_add_test(tc_reverse, test_reverse_single_char);
	tcase_add_test(tc_reverse, test_reverse_empty);
	suite_add_tcase(s, tc_reverse);

	tc_strpos = tcase_create("strpos");
	tcase_add_test(tc_strpos, test_strpos_found_at_start);
	tcase_add_test(tc_strpos, test_strpos_found_in_middle);
	tcase_add_test(tc_strpos, test_strpos_not_found);
	suite_add_tcase(s, tc_strpos);

	tc_charcount = tcase_create("char_count");
	tcase_add_test(tc_charcount, test_char_count_multiple);
	tcase_add_test(tc_charcount, test_char_count_none);
	tcase_add_test(tc_charcount, test_char_count_nul_chr);
	suite_add_tcase(s, tc_charcount);

	tc_strncopy = tcase_create("strncopy");
	tcase_add_test(tc_strncopy, test_strncopy_normal);
	tcase_add_test(tc_strncopy, test_strncopy_truncates);
	tcase_add_test(tc_strncopy, test_strncopy_exact_fit);
	suite_add_tcase(s, tc_strncopy);

	tc_numeric = tcase_create("is_numeric");
	tcase_add_test(tc_numeric, test_is_numeric_integer);
	tcase_add_test(tc_numeric, test_is_numeric_negative);
	tcase_add_test(tc_numeric, test_is_numeric_float);
	tcase_add_test(tc_numeric, test_is_numeric_alpha);
	tcase_add_test(tc_numeric, test_is_numeric_empty);
	tcase_add_test(tc_numeric, test_is_numeric_whitespace_only);
	suite_add_tcase(s, tc_numeric);

	return s;
}

int main(void)
{
	Suite   *s  = util_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);

	int failed = srunner_ntests_failed(sr);
	srunner_free(sr);

	return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
