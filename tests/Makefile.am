# +-------------------------------------------------------------------------+
# | Copyright (C) 2004-2026 The Cacti Group                                 |
# |                                                                         |
# | This program is free software; you can redistribute it and/or           |
# | modify it under the terms of the GNU General Public License             |
# | as published by the Free Software Foundation; either version 2          |
# | of the License, or (at your option) any later version.                  |
# +-------------------------------------------------------------------------+

if HAVE_CHECK

AM_CFLAGS  = @HARDENING_CFLAGS@ $(CHECK_CFLAGS)
AM_LDFLAGS = @HARDENING_LDFLAGS@

# Utility sources shared by both test binaries.  spine.c pulls in main() and
# heavy DB/SNMP state, so we compile only the files that contain the functions
# under test.
UTIL_OBJS = \
	$(top_builddir)/util.$(OBJEXT) \
	$(top_builddir)/error.$(OBJEXT)

check_PROGRAMS = test_util test_config
TESTS          = $(check_PROGRAMS)

test_util_SOURCES  = test_util.c
test_util_CFLAGS   = $(AM_CFLAGS) -I$(top_srcdir)
test_util_LDADD    = $(UTIL_OBJS) $(CHECK_LIBS)

test_config_SOURCES  = test_config.c
test_config_CFLAGS   = $(AM_CFLAGS) -I$(top_srcdir)
test_config_LDADD    = $(UTIL_OBJS) $(CHECK_LIBS)

endif
