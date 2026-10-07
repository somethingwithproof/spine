#!/bin/sh
# Structural guards for the remote push path's cross-vendor and batching rules.
set -eu

srcdir="${srcdir:-.}"
cd "$srcdir"

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

body=$(awk '/^void poller_push_data_to_main\(void\) \{/{f=1} f{print} f&&/^\}/{exit}' util.c)
# best-of-best builds the poller_output upsert in the build_*_poll_queries()
# helpers rather than in poll_host(), so check every function that does.
poll_host_body=$(awk '/^static void build_[a-z_]*poll_queries\(.*\{$/{f=1} f{print} f&&/^\}/{f=0}' poller.c)

[ -n "$body" ] || fail "could not find poller_push_data_to_main() in util.c"
[ -n "$poll_host_body" ] || fail "could not find the poll query builders in poller.c"

printf '%s\n' "$body" | grep -q 'AS rs ON DUPLICATE KEY UPDATE' &&
	fail "remote pushes must not use row-alias syntax selected from the local server"

printf '%s\n' "$body" | grep -q 'if (set.dbonupdate' &&
	fail "remote pushes must not branch on the local server version"

awk '/^static void push_flush_batch/{f=1} f{print} f&&/^\}/{exit}' util.c |
	grep -q 'if (!spine_appendf' ||
	fail "overflowed remote push batches must not be sent"

printf '%s\n' "$poll_host_body" | grep -q 'if (set\.dbonupdate' &&
	fail "poller_output must not select SQL syntax from the local server version"

printf '%s\n' "$poll_host_body" | grep -q 'output=VALUES(output)' ||
	fail "poller_output must use the cross-vendor upsert form"

exit 0
