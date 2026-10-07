#!/usr/bin/env bash
# Copyright (C) 2004-2026 The Cacti Group
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation; either version 2.1 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
# License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this library; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
#
# Run actual small-buffer production polling against the repository SQL schema.
# Borrowed fixture values have no live credentials or external targets.
set -euo pipefail
DEFAULT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ROOT="${SPINE_TEST_REPOSITORY_ROOT:-$DEFAULT_ROOT}"
[[ -f "$ROOT/src/poller.c" && -f "$ROOT/tests/snmpv3/db/init.sql" ]]
export COMPOSE_PROJECT_NAME="spine-persistence-${GITHUB_RUN_ID:-local}-$$"
COMPOSE=(docker compose -f "$ROOT/tests/snmpv3/docker-compose.yml")
IMAGE="$COMPOSE_PROJECT_NAME:fixture"
ARTIFACTS="$(mktemp -d)"
cleanup() {
  "${COMPOSE[@]}" down -v --remove-orphans >/dev/null 2>&1 || true
  docker image rm "$IMAGE" >/dev/null 2>&1 || true
  rm -rf "$ARTIFACTS"
}
trap cleanup EXIT

python3 - "$ARTIFACTS" "$ROOT" <<'PYFIXTURE'
from pathlib import Path
import hashlib,json,sys
out=Path(sys.argv[1]); root=Path(sys.argv[2])
sample=" ".join(f"metric{i:03d}:{i}" for i in range(1,281))
assert 2048 < len(sample) < 4096
out.joinpath("sample.txt").write_text(sample+"\n")
out.joinpath("producer").write_text("#!/bin/sh\nexec cat /artifacts/sample.txt\n")
out.joinpath("producer").chmod(0o755)
name="quoted'\\rrd"
rows=[]
for item in range(101,181):
 rows.append(f"({item},1,1,'127.0.0.1',CONVERT(0x{name.encode().hex()} USING utf8mb4),'/dev/null',1,600,0,'/artifacts/producer',{item+1000},1)")
rows.append(f"(999,1,1,'127.0.0.1',CONVERT(0x{name.encode().hex()} USING utf8mb4),'/dev/null',1,600,60,'/artifacts/producer',1,1)")
out.joinpath("seed.sql").write_text("TRUNCATE poller_output; TRUNCATE poller_output_boost; TRUNCATE poller_item; TRUNCATE host; INSERT INTO host (id,hostname,snmp_version,device_threads,availability_method,status,poller_id) VALUES (1,'127.0.0.1',0,1,0,3,1); INSERT INTO poller_item (local_data_id,host_id,action,hostname,rrd_name,rrd_path,rrd_num,rrd_step,rrd_next_step,arg1,snmp_port,poller_id) VALUES "+",".join(rows)+";\n")
out.joinpath("expected.sha256").write_text(hashlib.sha256(sample.encode()).hexdigest())
print(json.dumps({"sample_bytes":len(sample),"due_items":80,"deferred_item":999,"rrd_name_hex":name.encode().hex(),"source_sha256":hashlib.sha256(root.joinpath("src/poller.c").read_bytes()).hexdigest()}))
PYFIXTURE

docker build --target builder -t "$IMAGE" "$ROOT"
docker run --rm --entrypoint sh -v "$ARTIFACTS:/artifacts" "$IMAGE" -ec '
  cmake -S /src -B /src/build -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Debug \
    -DMAX_MYSQL_BUF_SIZE=4096 -DRESULTS_BUFFER=4096 \
    -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
  cmake --build /src/build --target spine -j4
  cp /src/build/spine /artifacts/spine
'
"${COMPOSE[@]}" up -d --wait db
sql() {
  local query="$1"
  "${COMPOSE[@]}" exec -T db mariadb -uspine -pspine cacti -N -e "$query"
}
expected_hash="$(cat "$ARTIFACTS/expected.sha256")"
for profiles in 1 2; do
  for boost in 0 1; do
    "${COMPOSE[@]}" exec -T db mariadb -uspine -pspine cacti < "$ARTIFACTS/seed.sql"
    sql "REPLACE INTO settings (name,value) VALUES ('active_profiles','$profiles'),('boost_redirect','$boost'),('boost_rrd_update_enable','$boost');"
    status=0
    docker run --rm --network "${COMPOSE_PROJECT_NAME}_default" \
      -v "$ROOT/tests/snmpv3/spine/spine.conf:/etc/spine/spine.conf:ro" \
      -v "$ARTIFACTS:/artifacts:ro" -e ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
      -e UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 --entrypoint timeout \
      "$IMAGE" 60 /artifacts/spine --conf=/etc/spine/spine.conf -f 1 -l 1 -S \
      > "$ARTIFACTS/poll.log" 2>&1 || status=$?
    if [[ $status -ne 0 ]]; then cat "$ARTIFACTS/poll.log"; exit 1; fi
    grep -q 'shutdown metrics all zero' "$ARTIFACTS/poll.log"
    if grep -qiE 'ERROR: AddressSanitizer|runtime error:|ERROR.*MySQL|Unknown column' "$ARTIFACTS/poll.log"; then
      cat "$ARTIFACTS/poll.log"; exit 1
    fi
    echo "Observed profiles=$profiles boost=$boost: $(sql 'SELECT COUNT(*),MIN(LENGTH(output)),MAX(LENGTH(output)),MIN(SHA2(output,256)),HEX(MIN(rrd_name)) FROM poller_output;') expected-hash=$expected_hash"
    expected_rows=80
    # The single-partition dispatcher limits by the 80 due items even
    # when profile1 omits the predicate. Distinct ports make selection
    # deterministic: deferred999 precedes due101..179; due180 is clipped.
    [[ "$(sql 'SELECT COUNT(*) FROM poller_output;')" == "$expected_rows" ]]
    [[ "$(sql "SELECT COUNT(*) FROM poller_output WHERE SHA2(output,256)='$expected_hash' AND rrd_name=CONVERT(0x71756f746564275c727264 USING utf8mb4);")" == "$expected_rows" ]]
    [[ "$(sql 'SELECT COUNT(DISTINCT time) FROM poller_output;')" == 1 ]]
    if [[ $profiles -eq 1 ]]; then
      [[ "$(sql 'SELECT COUNT(*) FROM poller_output WHERE local_data_id=999;')" == 1 ]]
      [[ "$(sql 'SELECT COUNT(*) FROM poller_output WHERE local_data_id BETWEEN 101 AND 179;')" == 79 ]]
      [[ "$(sql 'SELECT COUNT(*) FROM poller_output WHERE local_data_id=180;')" == 0 ]]
      [[ "$(sql 'SELECT COUNT(*) FROM poller_item WHERE local_data_id<>999 AND rrd_next_step=0;')" == 80 ]]
      [[ "$(sql 'SELECT rrd_next_step FROM poller_item WHERE local_data_id=999;')" == 60 ]]
    else
      [[ "$(sql 'SELECT COUNT(*) FROM poller_output WHERE local_data_id=999;')" == 0 ]]
      [[ "$(sql 'SELECT COUNT(*) FROM poller_output WHERE local_data_id BETWEEN 101 AND 180;')" == 80 ]]
      [[ "$(sql 'SELECT COUNT(*) FROM poller_item WHERE rrd_next_step=300;')" == 81 ]]
    fi
    if [[ $boost -eq 1 ]]; then
      [[ "$(sql 'SELECT COUNT(*) FROM poller_output_boost;')" == "$expected_rows" ]]
      [[ "$(sql 'SELECT COUNT(*) FROM poller_output p JOIN poller_output_boost b ON b.local_data_id=p.local_data_id AND BINARY b.rrd_name=BINARY p.rrd_name AND b.time=p.time AND BINARY b.output=BINARY p.output;')" == "$expected_rows" ]]
    else
      [[ "$(sql 'SELECT COUNT(*) FROM poller_output_boost;')" == 0 ]]
    fi
    [[ "$(sql 'SELECT COUNT(*) FROM host_errors;')" == 0 ]]
    echo "PERSISTENCE_PROFILES_${profiles}_BOOST_${boost}_PASS: $expected_rows exact samples, byte-preserved normal/Boost output, due-state and clean sanitizer shutdown"
  done
done
