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
"""Publish one stable check without concealing requested analysis failures."""
from __future__ import annotations

import os
from pathlib import Path


def result(policy: str, eligible: str, analysis: str, required: str) -> tuple[int, str]:
    if policy != "success":
        return 1, "Sonar policy failed or was cancelled."
    if eligible == "true":
        if analysis != "success":
            return 1, "Requested Sonar analysis or its quality gate did not succeed."
        return 0, "Sonar analysis and quality gate passed."
    if required == "true":
        return 1, "Required Sonar analysis was skipped. Enable analysis and use a trusted branch; fork secrets remain isolated."
    if analysis != "skipped":
        return 1, "Unexpected analysis result for an intentionally skipped job."
    return 0, "Sonar analysis intentionally skipped under the modernization policy."


def main() -> int:
    code, message = result(os.environ["POLICY_RESULT"], os.environ["ELIGIBLE"],
                           os.environ["ANALYSIS_RESULT"], os.environ.get("SONAR_REQUIRED", ""))
    print(("::error::" if code else "") + message)
    with Path(os.environ["GITHUB_STEP_SUMMARY"]).open("a") as summary:
        summary.write(message + "\n")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
