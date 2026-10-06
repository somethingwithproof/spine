# +-------------------------------------------------------------------------+
# | Copyright (C) 2004-2024 The Cacti Group                                 |
# |                                                                         |
# | This program is free software; you can redistribute it and/or           |
# | modify it under the terms of the GNU General Public License             |
# | as published by the Free Software Foundation; either version 2          |
# | of the License, or (at your option) any later version.                  |
# |                                                                         |
# | This program is distributed in the hope that it will be useful,         |
# | but WITHOUT ANY WARRANTY; without even the implied warranty of          |
# | MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           |
# | GNU General Public License for more details.                            |
# +-------------------------------------------------------------------------+
# | Cacti: The Complete RRDtool-based Graphing Solution                     |
# +-------------------------------------------------------------------------+
# | This code is designed, written, and maintained by the Cacti Group. See  |
# | about.php and/or the AUTHORS file for specific developer information.   |
# +-------------------------------------------------------------------------+
# | http://www.cacti.net/                                                   |
# +-------------------------------------------------------------------------+

AUTOMAKE_OPTIONS = foreign
ACLOCAL_AMFLAGS = -I m4

spine_shared_sources = sql.c util.c snmp.c locks.c poller.c nft_popen.c php.c ping.c keywords.c error.c
spine_SOURCES = $(spine_shared_sources) spine.c

noinst_HEADERS = common.h error.h keywords.h locks.h nft_popen.h php.h ping.h poller.h snmp.h spine.h sql.h util.h spine_types.h

configdir = $(sysconfdir)
dist_config_DATA = spine.conf.dist

bin_PROGRAMS = spine

man_MANS = spine.1
DISTCLEANFILES = spine.1

check_PROGRAMS = test_spine_regressions
dist_noinst_DATA = tests/fixtures/configuration.sql tests/fixtures/poller.sql tests/fixtures/snmpd.conf tests/fixtures/snmpv3-agent.conf tests/fixtures/snmpv3-users.conf
TESTS = $(check_PROGRAMS)
test_spine_regressions_SOURCES = $(spine_SOURCES) tests/test_spine_regressions.c tests/test_spine_contracts.c tests/test_spine_reindex_contracts.c tests/test_spine_snmpv3_contracts.c tests/test_spine_settings_contracts.c tests/test_spine_cli_contracts.c tests/test_spine_stream_contracts.c
# Link the real production translation units and globals, with a separate test
# entry point. The test source undefines main before declaring its own entry.
test_spine_regressions_CPPFLAGS = $(AM_CPPFLAGS) -I$(srcdir) -Dmain=spine_program_main

if HAVE_LINKER_WRAP
check_PROGRAMS += test_spine_faults
# Reuse the real renamed spine object for its production globals, avoiding
# another production main-renaming flag on the fault-test translation units.
test_spine_faults_SOURCES = $(spine_shared_sources) tests/test_spine_faults.c
test_spine_faults_CPPFLAGS = $(AM_CPPFLAGS) -I$(srcdir)
test_spine_faults_LDADD = test_spine_regressions-spine.$(OBJEXT)
test_spine_faults_LDFLAGS = $(AM_LDFLAGS) -Wl,--wrap=strftime -Wl,--wrap=mysql_query -Wl,--wrap=mysql_ping \
 -Wl,--wrap=pipe -Wl,--wrap=socketpair -Wl,--wrap=fork -Wl,--wrap=dup2 \
 -Wl,--wrap=execve -Wl,--wrap=malloc -Wl,--wrap=strdup
endif


spine.1: $(bin_PROGRAMS)
	$(HELP2MAN) --output=$@ --name='Data Collector for Cacti' --no-info --version-option='--version' ./spine

EXTRA_DIST = tests/tools/run_snmpv3.sh tests/tools/run_coverage.sh tests/tools/coverage_process_exit.h \
 tests/tools/verify_coverage.sh tests/tools/test_coverage_verifier.sh
.PHONY: coverage-source-list coverage-test-source-list
coverage-source-list:
	@printf '%s\n' $(spine_SOURCES)

coverage-test-source-list:
	@printf '%s\n' $(test_spine_regressions_SOURCES)

.PHONY: coverage-fault-source-list
coverage-fault-source-list:
	@printf '%s\n' $(test_spine_faults_SOURCES)
