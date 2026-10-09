#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2004-2026 The Cacti Group
# SPDX-License-Identifier: LGPL-2.1-or-later
# Fork maintenance: Thomas Vincent.
# Project contributor history: CONTRIBUTORS.md.
"""Decide analysis eligibility without accessing credentials or executing PR text."""
from __future__ import annotations

import json
import os
from pathlib import Path


def event_branch(event_name: str, event: dict, ref: str) -> tuple[str | None, str]:
    repository = event.get("repository", {})
    if event_name == "pull_request":
        head = event.get("pull_request", {}).get("head", {})
        if not repository.get("full_name") or head.get("repo", {}).get("full_name") != repository["full_name"]:
            return None, "Fork PR: analysis credential is isolated; normal CI still runs."
        return head.get("ref", ""), ""
    if event_name == "push" and ref.startswith("refs/heads/"):
        return ref.removeprefix("refs/heads/"), ""
    return None, "Event is outside the analysis policy."


def eligibility(event_name: str, event: dict, ref: str, enabled: str,
                actor: str, required: str = "") -> tuple[bool, str]:
    if enabled != "true":
        return False, "ENABLE_SONAR is absent or not true."
    if actor == "dependabot[bot]":
        return False, "Dependabot runs cannot receive the Sonar credential."
    if event_name == "workflow_dispatch":
        requested = event.get("inputs", {}).get("run_sonar", "true")
        return (True, "Manual analysis requested.") if requested in (True, "true") else (False, "Manual analysis disabled by input.")
    branch, reason = event_branch(event_name, event, ref)
    if branch is None:
        return False, reason
    if event_name == "pull_request" and required == "true":
        return True, "Required mode analyzes every trusted PR."
    if event_name == "push" and branch == event.get("repository", {}).get("default_branch"):
        return True, "Default branch analysis."
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
