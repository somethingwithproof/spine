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
"""Decide analysis eligibility without accessing credentials or executing PR text."""
from __future__ import annotations

import json
import os
from pathlib import Path


def eligibility(event_name: str, event: dict, ref: str, enabled: str,
                actor: str, required: str = "") -> tuple[bool, str]:
    if enabled != "true":
        return False, "ENABLE_SONAR is absent or not true."
    if actor == "dependabot[bot]":
        return False, "Dependabot runs cannot receive the Sonar credential."
    repository = event.get("repository", {})
    if event_name == "pull_request":
        head = event.get("pull_request", {}).get("head", {})
        if not repository.get("full_name") or head.get("repo", {}).get("full_name") != repository["full_name"]:
            return False, "Fork PR: analysis credential is isolated; normal CI still runs."
        branch = head.get("ref", "")
        if required == "true":
            return True, "Required mode analyzes every trusted PR."
    elif event_name == "workflow_dispatch":
        requested = event.get("inputs", {}).get("run_sonar", "true")
        return (True, "Manual analysis requested.") if requested in (True, "true") else (False, "Manual analysis disabled by input.")
    elif event_name == "push" and ref.startswith("refs/heads/"):
        branch = ref.removeprefix("refs/heads/")
        if branch == repository.get("default_branch"):
            return True, "Default branch analysis."
    else:
        return False, "Event is outside the analysis policy."
    if required == "true":
        return True, "Required mode analyzes every trusted branch."
    if branch.startswith(("sonar/", "release/")):
        return True, "Branch explicitly requests analysis."
    return False, "Ordinary development branch: analysis intentionally skipped."


def main() -> None:
    event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text())
    eligible, reason = eligibility(os.environ["GITHUB_EVENT_NAME"], event,
                                  os.environ.get("GITHUB_REF", ""),
                                  os.environ.get("ENABLE_SONAR", ""),
                                  os.environ.get("GITHUB_ACTOR", ""),
                                  os.environ.get("SONAR_REQUIRED", ""))
    with Path(os.environ["GITHUB_OUTPUT"]).open("a") as output:
        output.write(f"eligible={str(eligible).lower()}\n")
    with Path(os.environ["GITHUB_STEP_SUMMARY"]).open("a") as summary:
        summary.write(f"Sonar {'eligible' if eligible else 'skipped'}: {reason}\n")
    print(reason)


if __name__ == "__main__":
    main()
