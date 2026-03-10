#!/bin/sh
set -eu

COMPOSE="docker compose -f docker-compose.test.yml"

cleanup() {
    $COMPOSE down -v 2>/dev/null || true
}
trap cleanup EXIT INT TERM

$COMPOSE up -d db snmpsim
$COMPOSE up --exit-code-from spine-test spine-test

echo "Integration test passed."
