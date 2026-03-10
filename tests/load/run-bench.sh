#!/bin/sh
# Load benchmark for Cacti spine.
#
# Starts MariaDB + snmpsim via docker-compose.load.yml (port-mapped to the
# host), seeds the DB with N fake devices, then runs the local spine binary
# at multiple thread counts and prints a throughput table.
#
# Prerequisites:
#   - Python 3 in PATH
#   - Docker with Compose v2 (docker compose)
#   - spine binary built at ../../spine relative to this script
#
# Usage:
#   cd tests/load
#   sh run-bench.sh [--count N] [--threads T1,T2,T3]

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

SPINE_BIN="${SPINE_BIN:-${REPO_ROOT}/spine}"
COUNT="${COUNT:-500}"
THREAD_COUNTS="${THREAD_COUNTS:-10,25,50}"
SNMP_COMMUNITY="${SNMP_COMMUNITY:-public}"
SNMP_VERSION="${SNMP_VERSION:-2}"
SNMP_PORT=16100

DB_HOST="127.0.0.1"
DB_PORT="3307"
DB_USER="cactiuser"
DB_PASS="cactiuser"
DB_NAME="cacti"
DB_ROOT_PASS="rootpass"

COMPOSE="docker compose -f ${SCRIPT_DIR}/docker-compose.load.yml"
SEED_SQL="${SCRIPT_DIR}/load-seed.sql"
SPINE_CONF="${SCRIPT_DIR}/spine.conf.load"

# --- parse flags ------------------------------------------------------------

while [ $# -gt 0 ]; do
    case "$1" in
        --count)   COUNT="$2";        shift 2 ;;
        --threads) THREAD_COUNTS="$2"; shift 2 ;;
        *)         echo "Unknown flag: $1" >&2; exit 1 ;;
    esac
done

ITEMS=$(( COUNT * 5 ))   # 5 OIDs per device

# --- cleanup on exit --------------------------------------------------------

cleanup() {
    ${COMPOSE} down -v 2>/dev/null || true
    rm -f "${SEED_SQL}" "${SPINE_CONF}"
}
trap cleanup EXIT INT TERM

# --- preflight --------------------------------------------------------------

if [ ! -x "${SPINE_BIN}" ]; then
    echo "spine binary not found or not executable: ${SPINE_BIN}" >&2
    echo "Build spine first: cd ${REPO_ROOT} && ./configure && make" >&2
    exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 not found in PATH" >&2
    exit 1
fi

if ! docker compose version >/dev/null 2>&1; then
    echo "docker compose (v2) not found" >&2
    exit 1
fi

if ! command -v mysql >/dev/null 2>&1; then
    echo "mysql client not found in PATH (needed to seed the DB)" >&2
    exit 1
fi

# --- generate test data -----------------------------------------------------

echo "Generating ${COUNT} .snmprec files..."
python3 "${SCRIPT_DIR}/gen-snmprec.py" \
    --count "${COUNT}" \
    --outdir "${SCRIPT_DIR}/snmprec"

echo "Generating seed SQL..."
python3 "${SCRIPT_DIR}/gen-seed.py" \
    --count "${COUNT}" \
    --snmp-community "${SNMP_COMMUNITY}" \
    --snmp-version "${SNMP_VERSION}" \
    --snmp-port "${SNMP_PORT}" \
    > "${SEED_SQL}"

# --- write spine config -----------------------------------------------------

cat > "${SPINE_CONF}" <<EOF
DB_Host        ${DB_HOST}
DB_Port        ${DB_PORT}
DB_Database    ${DB_NAME}
DB_User        ${DB_USER}
DB_Pass        ${DB_PASS}
DB_PreG        0
Poller         1
EOF

# --- start infrastructure ---------------------------------------------------

echo "Starting MariaDB + snmpsim..."
${COMPOSE} up -d db snmpsim

echo "Waiting for MariaDB to be healthy..."
# Poll until the healthcheck passes (up to 60 s).
i=0
while [ $i -lt 60 ]; do
    status=$(docker inspect \
        --format='{{.State.Health.Status}}' \
        "$(${COMPOSE} ps -q db)" 2>/dev/null || true)
    if [ "${status}" = "healthy" ]; then
        break
    fi
    sleep 1
    i=$(( i + 1 ))
done
if [ "${status}" != "healthy" ]; then
    echo "MariaDB did not become healthy within 60 s" >&2
    exit 1
fi

# Give snmpsim a moment to load .snmprec files.
sleep 2

# --- seed the database ------------------------------------------------------

echo "Seeding database with ${COUNT} hosts and ${ITEMS} poller_items..."
mysql -h "${DB_HOST}" -P "${DB_PORT}" \
      -u root -p"${DB_ROOT_PASS}" \
      "${DB_NAME}" < "${SEED_SQL}"

# --- print table header -----------------------------------------------------

HR="--------+---------+-------+------------+---------+--------------------"
printf "\n%s\n" "${HR}"
printf "%-8s| %-8s| %-6s| %-11s| %-8s| %s\n" \
       "threads" "devices" "items" "duration_s" "results" "throughput (items/s)"
printf "%s\n" "${HR}"

# --- run benchmark ----------------------------------------------------------

# Convert comma-separated list to whitespace-separated.
thread_list="$(echo "${THREAD_COUNTS}" | tr ',' ' ')"

for threads in ${thread_list}; do

    # Truncate output table so each run starts clean.
    mysql -h "${DB_HOST}" -P "${DB_PORT}" \
          -u root -p"${DB_ROOT_PASS}" \
          "${DB_NAME}" \
          -e "TRUNCATE TABLE poller_output;" 2>/dev/null

    start=$(date +%s)

    # spine <poller_id> <concurrent_processes> [<poller_items>]
    # We pass the total item count as the ceiling so spine processes all rows.
    "${SPINE_BIN}" --conf "${SPINE_CONF}" \
        --poller_id 1 \
        --threads "${threads}" \
        1 1 2>/dev/null || true

    end=$(date +%s)
    elapsed=$(( end - start ))

    results=$(mysql -h "${DB_HOST}" -P "${DB_PORT}" \
        -u root -p"${DB_ROOT_PASS}" \
        "${DB_NAME}" \
        -sNe "SELECT COUNT(*) FROM poller_output;" 2>/dev/null || echo 0)

    # Verify spine wrote a result row for every polled item.
    if [ "${results}" -ne "${ITEMS}" ]; then
        printf "FAIL: expected %s rows in poller_output, got %s (threads=%s)\n" \
               "${ITEMS}" "${results}" "${threads}" >&2
        exit 1
    fi

    if [ "${elapsed}" -gt 0 ]; then
        throughput=$(( results / elapsed ))
    else
        # Sub-second run -- use a decimal approximation via awk.
        throughput=$(awk "BEGIN { printf \"%.1f\", ${results} / 0.5 }")
    fi

    printf "%-8s| %-8s| %-6s| %-11s| %-8s| %s\n" \
           "${threads}" "${COUNT}" "${ITEMS}" \
           "${elapsed}" "${results}" "${throughput}"
done

printf "%s\n\n" "${HR}"

echo "Benchmark complete. Docker services will be torn down on exit."
