#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2004-2026 The Cacti Group
# SPDX-License-Identifier: LGPL-2.1-or-later
# Fork maintenance: Thomas Vincent.
# Project contributor history: CONTRIBUTORS.md.
"""Publish one stable check without concealing requested analysis failures."""
from __future__ import annotations

import os
from pathlib import Path


def result(policy: str, eligible: str, analysis: str, required: str) -> tuple[int, str]:
    if policy != "success":
        return 1, "Sonar policy failed or was cancelled."
    if eligible not in ("true", "false"):
        return 1, "Sonar policy eligibility output is missing or malformed."
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
