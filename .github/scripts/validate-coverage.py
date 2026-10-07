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
"""Fail closed on missing or stale actual-producer coverage evidence."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import xml.etree.ElementTree as ET


def validate(root: Path) -> None:
    manifest = json.loads((root / "coverage/producer.json").read_text())
    expected = {f"NATIVE_COVERAGE_PROFILE_{profile}_BOOST_{boost}_PASS"
                for profile in (1, 2) for boost in (0, 1)}
    expected.add("NATIVE_COVERAGE_INVALID_SCRIPT_PASS")
    if manifest.get("producer") != "coverage-native" or set(manifest.get("cases", [])) != expected:
        raise ValueError("Missing native producer scenarios")
    actual_sources = {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
                      for path in sorted((root / "src").rglob("*")) if path.is_file()}
    if manifest.get("sources") != actual_sources:
        raise ValueError("Missing or stale worker/production source registrations")
    files = {file.attrib["path"]: file for file in ET.parse(root / "coverage/sonar.xml").getroot().findall("file")}
    entries = json.loads((root / "build/compile_commands.json").read_text())
    compiled = {}
    for entry in entries:
        source = Path(entry["file"])
        if not source.is_absolute():
            source = Path(entry["directory"]) / source
        try:
            relative = str(source.resolve().relative_to(root.resolve()))
        except ValueError:
            continue
        if relative.startswith("src/") and source.suffix == ".c":
            compiled.setdefault(relative, entry)
    if not compiled:
        raise ValueError("Compilation database has no production units")
    for source in sorted(set(compiled) - set(files)):
        # OS-guarded empty units still compile. Accept omission only when the
        # actual compiler's coverage tool proves no executable lines; no
        # source allowlist or guessed platform exclusion is used.
        entry = compiled[source]
        output = Path(entry.get("output", ""))
        if not output.is_absolute():
            output = Path(entry["directory"]) / output
        notes = output.with_suffix(".gcno")
        if not notes.is_file():
            raise ValueError("Missing coverage report/notes for " + source)
        tool = shlex.split(os.environ.get("GCOV", "gcov"))
        report = subprocess.run(tool + ["-n", str(notes)], capture_output=True,
                                text=True, timeout=30, check=True)
        # GCC emits the explicit marker. LLVM's gcov emits no output for
        # a successfully parsed function-free unit (verified with actual
        # guarded .gcno files); non-empty units emit file/line summaries.
        empty_unit = "No executable lines" in report.stdout or (not report.stdout.strip() and not report.stderr.strip())
        if not empty_unit:
            raise ValueError("Compiled production coverage is omitted: " + source)
    for source in ("src/spine.c", "src/poller.c"):
        if source not in files or not any(line.attrib["covered"] == "true" for line in files[source].findall("lineToCover")):
            raise ValueError("Actual production entrypoint/poller coverage is missing")
        actual = hashlib.sha256((root / source).read_bytes()).hexdigest()
        if manifest.get("sources", {}).get(source) != actual:
            raise ValueError("Coverage source revision does not match producer evidence")
    for path, key in (("build/spine", "binary_sha256"),
                      ("build/compile_commands.json", "compile_commands_sha256"),
                      ("tests/snmpv3/db/init.sql", "schema_sha256"),
                      (".github/scripts/coverage-native.py", "runner_sha256")):
        if hashlib.sha256((root / path).read_bytes()).hexdigest() != manifest.get(key):
            raise ValueError("Producer binary/schema/tooling does not match collection")


if __name__ == "__main__":
    validate(Path(__file__).resolve().parents[2])
    print("Native producer provenance and production coverage verified.")
