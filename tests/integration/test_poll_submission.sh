#!/usr/bin/env bash
# Exercise rejection through the production CLI and the owned SNMP/SQL fixture.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
export COMPOSE_PROJECT_NAME="spine-submission-${GITHUB_RUN_ID:-local}-$$"
COMPOSE=(docker compose -f "$ROOT/tests/snmpv3/docker-compose.yml")
IMAGE="$COMPOSE_PROJECT_NAME:fixture"
ARTIFACTS="$(mktemp -d)"
cleanup() {
  "${COMPOSE[@]}" down -v --remove-orphans >/dev/null 2>&1 || true
  docker image rm "$IMAGE" >/dev/null 2>&1 || true
  rm -rf "$ARTIFACTS"
}
trap cleanup EXIT

docker build --target builder -t "$IMAGE" "$ROOT"
docker run --rm --entrypoint cc -v "$ARTIFACTS:/artifacts" "$IMAGE" \
  -std=c17 -shared -fPIC -Wall -Wextra -Werror \
  /src/tests/integration/poll_submission_faults.c -ldl -o /artifacts/faults.so
"${COMPOSE[@]}" up -d --wait db snmpd
sql() { "${COMPOSE[@]}" exec -T db mariadb -uspine -pspine cacti -N -e "$1"; }
run_poll() {
  docker run --rm --network "${COMPOSE_PROJECT_NAME}_default" \
    -v "$ROOT/tests/snmpv3/spine/spine.conf:/etc/spine/spine.conf:ro" \
    -v "$ARTIFACTS/faults.so:/faults.so:ro" \
    -e LD_PRELOAD=/faults.so -e "SPINE_TEST_SUBMISSION_FAULT=$1" \
    --entrypoint timeout "$IMAGE" 30 /usr/local/bin/spine \
    --conf=/etc/spine/spine.conf -f 1 -l 1 -S -M
}
for mode in queue wake; do
  sql "TRUNCATE poller_output; UPDATE host SET total_polls=0 WHERE id=1;"
  status=0
  run_poll "$mode" >"$ARTIFACTS/$mode.log" 2>&1 || status=$?
  cat "$ARTIFACTS/$mode.log"
  [[ $status -eq 1 ]] || { echo "Expected exit 1 for $mode; got $status" >&2; exit 1; }
  if [[ $mode == queue ]]; then
    grep -q 'FIXTURE: rejected uv_queue_work' "$ARTIFACTS/$mode.log"
  else
    grep -q 'FIXTURE: rejected first uv_async_send' "$ARTIFACTS/$mode.log"
  fi
  grep -q 'shutdown metrics all zero' "$ARTIFACTS/$mode.log"
  [[ "$(sql 'SELECT COUNT(*) FROM poller_output;')" == 0 ]]
  [[ "$(sql 'SELECT total_polls FROM host WHERE id=1;')" == 0 ]]
  echo "PASS: $mode rejection exits unsuccessfully, drains, and preserves pending work"
done
sql "TRUNCATE poller_output; UPDATE host SET total_polls=0,snmp_version=2,snmp_community='public' WHERE id=1; UPDATE poller_item SET snmp_version=2,snmp_community='public' WHERE host_id=1;"
run_poll none >"$ARTIFACTS/success.log" 2>&1
cat "$ARTIFACTS/success.log"
grep -q 'shutdown metrics all zero' "$ARTIFACTS/success.log"
[[ "$(sql 'SELECT COUNT(*) FROM poller_output WHERE local_data_id=1 AND output REGEXP "^[0-9]+$";')" == 1 ]]
[[ "$(sql 'SELECT total_polls FROM host WHERE id=1;')" -gt 0 ]]
echo 'PASS: admitted production poll persists real SNMP data and drains'

# Compile the native lifecycle fixture from the same production source/link
# configuration. No DB-less CTest can substitute for these producer checks.
docker run --rm --entrypoint sh -v "$ARTIFACTS:/artifacts" "$IMAGE" -ec '
  cmake -S /src -B /src/build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
  cmake --build /src/build --target test_native_partition_lifecycle -j4
  cp /src/build/test_native_partition_lifecycle /artifacts/lifecycle
'
for scenario in partition-order early-stop; do
  sql "TRUNCATE poller_output; TRUNCATE poller_item; TRUNCATE host;"
  if [[ $scenario == partition-order ]]; then
    sql "INSERT INTO host (id,hostname,snmp_version,device_threads,availability_method,status,poller_id) VALUES (1,'127.0.0.1',0,2,0,3,1);
      INSERT INTO poller_item (local_data_id,host_id,action,hostname,rrd_name,rrd_path,rrd_num,rrd_step,arg1,poller_id) VALUES
      (41,1,1,'127.0.0.1','value','/dev/null',1,300,'/artifacts/lifecycle --fixture-producer 41',1),
      (42,1,1,'127.0.0.1','value','/dev/null',1,300,'/artifacts/lifecycle --fixture-producer 42',1);"
    marker=NATIVE_LIFECYCLE_PARTITION_ORDER_PASS
  else
    sql "INSERT INTO host (id,hostname,snmp_version,device_threads,availability_method,status,poller_id) VALUES
      (1,'127.0.0.1',0,1,0,3,1),(2,'127.0.0.1',0,1,0,3,1);
      INSERT INTO poller_item (local_data_id,host_id,action,hostname,rrd_name,rrd_path,rrd_num,rrd_step,arg1,poller_id) VALUES
      (51,1,1,'127.0.0.1','value','/dev/null',1,300,'/artifacts/lifecycle --fixture-producer 51',1),
      (52,2,1,'127.0.0.1','value','/dev/null',1,300,'/artifacts/lifecycle --fixture-producer 52',1);"
    marker=NATIVE_LIFECYCLE_EARLY_STOP_PASS
  fi
  status=0
  docker run --rm --network "${COMPOSE_PROJECT_NAME}_default" \
    -v "$ROOT/tests/snmpv3/spine/spine.conf:/etc/spine/spine.conf:ro" \
    -v "$ARTIFACTS:/artifacts:ro" -e "SPINE_NATIVE_LIFECYCLE_SCENARIO=$scenario" \
    -e ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 -e UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    --entrypoint timeout "$IMAGE" 45 /artifacts/lifecycle \
    --conf=/etc/spine/spine.conf -f 1 -l 2 -S -M >"$ARTIFACTS/$scenario.log" 2>&1 || status=$?
  cat "$ARTIFACTS/$scenario.log"
  [[ $status -eq 0 ]] || { echo "Native $scenario exited $status" >&2; exit 1; }
  grep -q "$marker" "$ARTIFACTS/$scenario.log"
  grep -q 'shutdown metrics all zero' "$ARTIFACTS/$scenario.log"
  if [[ $scenario == partition-order ]]; then
    [[ "$(sql 'SELECT COUNT(*) FROM poller_output WHERE (local_data_id=41 AND output="41") OR (local_data_id=42 AND output="42");')" == 2 ]]
    [[ "$(sql 'SELECT COUNT(*) FROM host WHERE id=1 AND polling_time>0;')" == 1 ]]
  else
    [[ "$(sql 'SELECT COUNT(*) FROM poller_output WHERE local_data_id=51 AND output="51";')" == 1 ]]
    [[ "$(sql 'SELECT COUNT(*) FROM poller_output WHERE local_data_id=52;')" == 0 ]]
    [[ "$(sql 'SELECT total_polls FROM host WHERE id=2;')" == 0 ]]
  fi
  echo "PASS: $scenario executes real producers with verified ownership and persisted outcomes"
done
