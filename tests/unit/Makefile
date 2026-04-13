CC      := cc
BINDIR  := build

SPINE_ROOT := ../..

# Use pkg-config if available, otherwise fallback to system defaults
MARIADB_CFLAGS := $(shell pkg-config --cflags mariadb 2>/dev/null || echo "-I/usr/include/mariadb")
NETSNMP_CFLAGS := $(shell net-snmp-config --cflags 2>/dev/null || echo "-I/usr/include/net-snmp")

CFLAGS := \
	-std=gnu17 \
	-DHAVE_CONFIG_H \
	-I$(SPINE_ROOT) \
	-I$(SPINE_ROOT)/config \
	$(MARIADB_CFLAGS) \
	$(NETSNMP_CFLAGS) \
	-Wall \
	-Wextra \
	-Wno-unused-parameter

LDFLAGS := \
	-lcmocka \
	-lm \
	-lmariadb

TARGETS := $(BINDIR)/test_build_fixes $(BINDIR)/test_util_strings

.PHONY: all build_dir build_binaries run clean

all: run

build_dir:
	mkdir -p $(BINDIR)

build_binaries: build_dir $(TARGETS)

$(BINDIR)/test_build_fixes: test_build_fixes.c | build_dir
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

$(BINDIR)/test_util_strings: test_util_strings.c | build_dir
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

run: build_binaries
	$(BINDIR)/test_build_fixes
	$(BINDIR)/test_util_strings

clean:
	rm -rf $(BINDIR)
