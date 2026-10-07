#!/usr/bin/env python3
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
# Copyright (C) 2004-2026 The Cacti Group

# SPDX-License-Identifier: LGPL-2.1-or-later
"""Execute the instrumented production CLI against an owned repository schema."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def run(args, **kwargs):
    return subprocess.run(args, check=True, text=True, timeout=180, **kwargs)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    root = Path(__file__).resolve().parents[2]
    binary = root / "build/spine"
    fixture = root / "tests/snmpv3/docker-compose.yml"
    schema = root / "tests/snmpv3/db/init.sql"
    project = f"spine-coverage-{os.getpid()}"
    sample = " ".join(f"metric{i:03d}:{i}" for i in range(1, 141))
    assert 1200 < len(sample) < 2048
    expected = hashlib.sha256(sample.encode()).hexdigest()
    cases = []
    with tempfile.TemporaryDirectory(prefix="spine-coverage-") as directory:
        temp = Path(directory)
        override = temp / "compose.yml"
        override.write_text('services:\n  db:\n    ports:\n      - "127.0.0.1::3306"\n')
        compose = ["docker", "compose", "-p", project, "-f", str(fixture), "-f", str(override)]

        def sql(query):
            return run(compose + ["exec", "-T", "db", "mariadb", "-uspine", "-pspine",
                                 "cacti", "-N", "-e", query], capture_output=True).stdout.strip()

        try:
            run(compose + ["up", "-d", "--wait", "--wait-timeout", "120", "db"])
            address = run(compose + ["port", "db", "3306"], capture_output=True).stdout.strip()
            if not address.startswith("127.0.0.1:") or not address.split(":")[1].isdigit():
                raise RuntimeError("Fixture DB must publish on a loopback port")
            config = temp / "spine.conf"
            config.write_text("DB_Host 127.0.0.1\nDB_Database cacti\nDB_User spine\nDB_Pass spine\nDB_Port " + address.split(":")[1] + "\n")
            config.chmod(0o600)
            (temp / "sample").write_text(sample + "\n")
            producer = temp / "producer"
            producer.write_text("#!/bin/sh\nexec cat " + shlex.quote(str(temp / "sample")) + "\n")
            producer.chmod(0o700)
            # Distinct SNMP ports preserve the original profile1 LIMIT selection.
            rows = [f"({item},1,1,'127.0.0.1',CONVERT(0x71756f746564275c727264 USING utf8mb4),'/dev/null',1,600,0,'{producer}',{item+1000},1)"
                    for item in range(101, 229)]
            rows.append(f"(999,1,1,'127.0.0.1',CONVERT(0x71756f746564275c727264 USING utf8mb4),'/dev/null',1,600,60,'{producer}',1,1)")
            seed = "TRUNCATE poller_output; TRUNCATE poller_output_boost; TRUNCATE poller_item; TRUNCATE host; TRUNCATE host_errors; INSERT INTO host (id,hostname,snmp_version,device_threads,availability_method,status,poller_id) VALUES (1,'127.0.0.1',0,1,0,3,1); INSERT INTO poller_item (local_data_id,host_id,action,hostname,rrd_name,rrd_path,rrd_num,rrd_step,rrd_next_step,arg1,snmp_port,poller_id) VALUES " + ",".join(rows)
            for profiles in (1, 2):
                for boost in (0, 1):
                    sql(seed)
                    sql(f"REPLACE INTO settings (name,value) VALUES ('active_profiles','{profiles}'),('boost_redirect','{boost}'),('boost_rrd_update_enable','{boost}');")
                    result = run([str(binary), "--conf=" + str(config), "-f", "1", "-l", "1", "-S"], capture_output=True)
                    if "shutdown metrics all zero" not in result.stdout + result.stderr:
                        raise RuntimeError("Instrumented CLI did not drain owned resources")
                    if sql("SELECT COUNT(*) FROM poller_output") != "128":
                        raise RuntimeError("Missing production outputs")
                    if sql(f"SELECT COUNT(*) FROM poller_output WHERE SHA2(output,256)='{expected}' AND HEX(rrd_name)='71756F746564275C727264'") != "128":
                        raise RuntimeError("Production output bytes differ from the actual producer")
                    if profiles == 1:
                        if sql("SELECT COUNT(*) FROM poller_output WHERE local_data_id=999") != "1" or sql("SELECT COUNT(*) FROM poller_output WHERE local_data_id=228") != "0":
                            raise RuntimeError("Profile1 selection limit changed")
                        if sql("SELECT COUNT(*) FROM poller_item WHERE local_data_id<>999 AND rrd_next_step=0") != "128" or sql("SELECT rrd_next_step FROM poller_item WHERE local_data_id=999") != "60":
                            raise RuntimeError("Profile1 schedule changed")
                    else:
                        if sql("SELECT COUNT(*) FROM poller_output WHERE local_data_id BETWEEN 101 AND 228") != "128" or sql("SELECT COUNT(*) FROM poller_item WHERE rrd_next_step=300") != "129":
                            raise RuntimeError("Profile2 selection or schedule changed")
                    if sql("SELECT COUNT(*) FROM poller_output_boost") != str(128 * boost):
                        raise RuntimeError("Boost output count changed")
                    if boost and sql("SELECT COUNT(*) FROM poller_output p JOIN poller_output_boost b ON p.local_data_id=b.local_data_id AND BINARY p.rrd_name=BINARY b.rrd_name AND p.time=b.time AND BINARY p.output=BINARY b.output") != "128":
                        raise RuntimeError("Normal/Boost output bytes diverge")
                    if sql("SELECT COUNT(*) FROM host_errors") != "0":
                        raise RuntimeError("Unexpected host errors")
                    marker = f"NATIVE_COVERAGE_PROFILE_{profiles}_BOOST_{boost}_PASS"
                    print(marker, flush=True)
                    cases.append(marker)
            # Invalid real producer output must reach normalization and persist
            # an undefined sample with the actual item identity in host_errors.
            sql(seed)
            sql("DELETE FROM poller_item WHERE local_data_id<>101; REPLACE INTO settings (name,value) VALUES ('active_profiles','1'),('boost_redirect','0'),('boost_rrd_update_enable','0');")
            (temp / "sample").write_text("not_numeric\n")
            result = run([str(binary), "--conf=" + str(config), "-f", "1", "-l", "1", "-S"], capture_output=True)
            if "shutdown metrics all zero" not in result.stdout + result.stderr:
                raise RuntimeError("Invalid-response producer did not drain")
            if sql("SELECT COUNT(*) FROM poller_output WHERE local_data_id=101 AND output='U'") != "1":
                raise RuntimeError("Invalid response must persist one undefined sample")
            if sql("SELECT COUNT(*) FROM host_errors WHERE host_id=1 AND errors=1 AND local_data_ids='101'") != "1":
                raise RuntimeError("Invalid-response error attribution changed")
            marker = "NATIVE_COVERAGE_INVALID_SCRIPT_PASS"
            print(marker, flush=True)
            cases.append(marker)
            output = root / "coverage"
            output.mkdir(exist_ok=True)
            manifest = {"producer": "coverage-native", "cases": cases,
                        "binary_sha256": digest(binary), "schema_sha256": digest(schema),
                        "runner_sha256": digest(Path(__file__)),
                        "sample_sha256": expected, "sample_bytes": len(sample),
                        "sources": {str(p.relative_to(root)): digest(p) for p in sorted((root / "src").rglob("*")) if p.is_file()},
                        "compile_commands_sha256": digest(root / "build/compile_commands.json")}
            (output / "producer.json").write_text(json.dumps(manifest, indent=2) + "\n")
        finally:
            subprocess.run(compose + ["down", "-v", "--remove-orphans"], check=True, timeout=180)


if __name__ == "__main__":
    main()
