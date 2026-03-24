# +-------------------------------------------------------------------------+
# | Copyright (C) 2004-2026 The Cacti Group                                 |
# | Integration test Makefile for spine                                     |
# +-------------------------------------------------------------------------+

TOP_SRCDIR = ../..

CC       ?= gcc
CFLAGS   ?= -std=c99 -Wall -Wextra -g

# Use mysql_config/mariadb_config if available, fall back to pkg-config, then Homebrew
MYSQL_CFLAGS := $(shell mysql_config --cflags 2>/dev/null || mariadb_config --cflags 2>/dev/null || echo "-I/opt/homebrew/opt/mysql-client/include/mysql")
MYSQL_LIBS   := $(shell mysql_config --libs 2>/dev/null || mariadb_config --libs 2>/dev/null || echo "-L/opt/homebrew/opt/mysql-client/lib -lmysqlclient")

SNMP_CFLAGS  := $(shell net-snmp-config --cflags 2>/dev/null || echo "-I/opt/homebrew/opt/net-snmp/include")
SNMP_LIBS    := $(shell net-snmp-config --libs 2>/dev/null || echo "-L/opt/homebrew/opt/net-snmp/lib -lnetsnmp")

CMOCKA_CFLAGS := $(shell pkg-config --cflags cmocka 2>/dev/null)
CMOCKA_LIBS   := $(shell pkg-config --libs cmocka 2>/dev/null)

SSL_CFLAGS   := $(shell pkg-config --cflags openssl 2>/dev/null || echo "-I/opt/homebrew/opt/openssl@3/include")
SSL_LIBS     := $(shell pkg-config --libs openssl 2>/dev/null || echo "-L/opt/homebrew/opt/openssl@3/lib -lcrypto -lssl")

CPPFLAGS = -I$(TOP_SRCDIR) $(MYSQL_CFLAGS) $(SNMP_CFLAGS) $(SSL_CFLAGS) $(CMOCKA_CFLAGS)
LDFLAGS  =
LIBS     = $(MYSQL_LIBS) $(SNMP_LIBS) $(SSL_LIBS) $(CMOCKA_LIBS) -lm -lpthread

# Spine object files needed by the tests
SPINE_OBJS_CORE = $(TOP_SRCDIR)/sql.o \
                  $(TOP_SRCDIR)/util.o \
                  $(TOP_SRCDIR)/locks.o \
                  $(TOP_SRCDIR)/error.o \
                  $(TOP_SRCDIR)/keywords.o

SPINE_OBJS_POLLER = $(SPINE_OBJS_CORE) \
                    $(TOP_SRCDIR)/poller.o \
                    $(TOP_SRCDIR)/snmp.o \
                    $(TOP_SRCDIR)/php.o \
                    $(TOP_SRCDIR)/ping.o \
                    $(TOP_SRCDIR)/nft_popen.o

STUBS = test_stubs.o

TESTS = test_db_connectivity \
        test_sql_buffer_integration \
        test_config_parsing \
        test_connection_pool \
        test_poller_pipeline

.PHONY: all clean run

all: $(TESTS)
	chmod +x run_all.sh

test_stubs.o: test_stubs.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

test_db_connectivity: test_db_connectivity.c integration_helpers.h $(STUBS)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ $< $(STUBS) $(SPINE_OBJS_CORE) $(LIBS)

test_sql_buffer_integration: test_sql_buffer_integration.c integration_helpers.h $(STUBS)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ $< $(STUBS) $(SPINE_OBJS_CORE) $(LIBS)

test_config_parsing: test_config_parsing.c integration_helpers.h $(STUBS)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ $< $(STUBS) $(SPINE_OBJS_CORE) $(LIBS)

test_connection_pool: test_connection_pool.c integration_helpers.h $(STUBS)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ $< $(STUBS) $(SPINE_OBJS_CORE) $(LIBS)

test_poller_pipeline: test_poller_pipeline.c integration_helpers.h $(STUBS)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ $< $(STUBS) $(SPINE_OBJS_CORE) $(TOP_SRCDIR)/poller.o $(TOP_SRCDIR)/snmp.o $(TOP_SRCDIR)/ping.o $(TOP_SRCDIR)/nft_popen.o $(LIBS)

run: all
	./run_all.sh

clean:
	rm -f $(TESTS) $(STUBS)
