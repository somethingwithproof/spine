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

# SPDX-License-Identifier: LGPL-2.1-or-later
"""Compare diagnostic identities and multiplicities without line-number churn."""
from collections import Counter
from pathlib import Path
import re
import sys

DIAGNOSTIC = re.compile(r"^([^:]+):[0-9]+:[0-9]+: (.*)$")


def diagnostics(text: str) -> Counter:
    result = Counter()
    # cppcheck may print identical configurations repeatedly. Match the old
    # sort -u contract before counting distinct diagnostic locations.
    for line in set(text.splitlines()):
        match = DIAGNOSTIC.match(line)
        if match and not match.group(2).startswith("note:"):
            result[(match.group(1), match.group(2))] += 1
    return result


def regressions(report: str, baseline: str) -> Counter:
    reference = diagnostics(baseline)
    if not reference:
        raise ValueError("Cppcheck baseline has no registered diagnostics")
    return diagnostics(report) - reference


def main() -> int:
    report = Path(sys.argv[1]).read_text()
    baseline = Path(sys.argv[2]).read_text()
    findings = regressions(report, baseline)
    lines = [f"{path}: {message}" for (path, message), count in sorted(findings.items())
             for _ in range(count)]
    Path(sys.argv[3]).write_text("\n".join(lines) + ("\n" if lines else ""))
    if findings:
        print("New cppcheck findings not in baseline:")
        print("\n".join(lines))
        return 1
    print("Cppcheck diagnostic identities and counts match the baseline.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
