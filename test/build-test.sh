#!/bin/sh
# build-test.sh - Verify spine compiles on the current platform.
# Exit 0 on success, non-zero on failure.
# Expects to be run from the spine source root.
set -eu

log() { printf '[build-test] %s\n' "$*"; }
fail() { log "FAIL: $*"; exit 1; }

log "Platform: $(uname -srm)"
log "Compiler: $(${CC:-cc} --version 2>&1 | head -1)"

# Step 1: bootstrap autotools
log "Running autoreconf..."
autoreconf -fi || fail "autoreconf failed"

# Step 2: configure with C11
log "Running configure..."
./configure || fail "configure failed"

# Verify C11 was selected
if grep -q 'std=.*11' config.log 2>/dev/null; then
	log "C11 standard confirmed in config.log"
else
	log "WARNING: Could not confirm C11 in config.log"
fi

# Step 3: compile (build binary; man page may fail without help2man)
log "Running make clean && make..."
make clean 2>/dev/null
NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)
make -j"$NPROC" spine 2>/dev/null || make -j"$NPROC" || fail "make failed"

# Step 4: smoke test
if [ -x ./spine ]; then
	log "Binary exists: $(file ./spine)"
	./spine --version 2>&1 | head -1 || true
	log "PASS"
else
	fail "spine binary not found after make"
fi
