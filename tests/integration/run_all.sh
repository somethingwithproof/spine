#!/bin/sh
set -eu

PASS=0
FAIL=0
TOTAL=0

run_test() {
    TOTAL=$((TOTAL + 1))
    printf "Running %s... " "$1"
    if ./"$1" 2>&1; then
        PASS=$((PASS + 1))
        printf "PASSED\n"
    else
        FAIL=$((FAIL + 1))
        printf "FAILED\n"
    fi
}

echo "=== Spine Integration Tests ==="
echo "DB_HOST=${DB_HOST:-localhost} DB_NAME=${DB_NAME:-cacti_test}"
echo ""

run_test test_db_connectivity
run_test test_sql_buffer_integration
run_test test_config_parsing
run_test test_connection_pool
run_test test_poller_pipeline

echo ""
echo "=== Results: ${PASS}/${TOTAL} passed, ${FAIL} failed ==="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
