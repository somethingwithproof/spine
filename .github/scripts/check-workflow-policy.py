#!/usr/bin/env python3
"""Enforce workflow hygiene policy on GitHub Actions files."""

from __future__ import annotations

import re
import sys
from pathlib import Path

import yaml


PINNED_REF_RE = re.compile(r"^[0-9a-f]{40}$")
CURL_PIPE_RE = re.compile(r"curl\b[^\n|]*\|\s*(?:sh|bash)\b")
# Accept either the strict bash form or the POSIX-sh-compatible 'set -eu'.
# Container steps on minimal images (alpine uses ash, some Debian fragments
# run under dash) cannot use 'pipefail' because dash/ash do not implement it.
ACCEPTED_FIRST_LINES = ("set -euo pipefail", "set -eu")
STRICT_LINE = "set -euo pipefail"
WORKFLOW_GLOB = ".github/workflows/*"
ALLOWLIST_CURL_PIPE = {}


def normalize_steps(job: dict) -> list[dict]:
	steps = job.get("steps")
	return steps if isinstance(steps, list) else []


def check_uses(path: str, step_name: str, uses_value: str, violations: list[str]) -> None:
	if uses_value.startswith("./") or uses_value.startswith("docker://"):
		return

	if "@" not in uses_value:
		violations.append(f"{path}:{step_name}: uses reference is missing @ref: {uses_value}")
		return

	ref = uses_value.split("@", 1)[1]
	if not PINNED_REF_RE.fullmatch(ref):
		violations.append(f"{path}:{step_name}: action ref must be a pinned SHA: {uses_value}")


def check_run(path: str, step_name: str, run_value: str, violations: list[str]) -> None:
	lines = [ln.strip() for ln in run_value.splitlines() if ln.strip()]
	if not lines:
		return

	if len(run_value.splitlines()) > 1:
		if lines[0] not in ACCEPTED_FIRST_LINES:
			violations.append(f"{path}:{step_name}: multiline run must start with one of {ACCEPTED_FIRST_LINES}")

	for match in CURL_PIPE_RE.finditer(run_value):
		_ = match
		allow_tokens = ALLOWLIST_CURL_PIPE.get(path, [])
		if not any(token in run_value for token in allow_tokens):
			violations.append(f"{path}:{step_name}: curl|sh is not allowlisted")


def audit(root: Path) -> list[str]:
	workflow_files = sorted(
		p for p in root.glob(WORKFLOW_GLOB) if p.suffix in (".yml", ".yaml")
	)
	violations: list[str] = []
	if not workflow_files:
		return ["No workflow files found; policy cannot validate an empty repository."]

	for wf in workflow_files:
		rel = str(wf.relative_to(root))
		try:
			doc = yaml.safe_load(wf.read_text(encoding="utf-8"))
		except Exception as exc:  # pragma: no cover
			violations.append(f"{rel}: failed to parse YAML: {exc}")
			continue

		jobs = doc.get("jobs", {}) if isinstance(doc, dict) else {}
		if not isinstance(jobs, dict) or not jobs:
			violations.append(f"{rel}: non-empty jobs mapping is required")
			continue
		permissions = doc.get("permissions")
		if not isinstance(permissions, dict) or permissions.get("contents") != "read" or "write" in permissions.values():
			violations.append(f"{rel}: default permissions must be contents: read, with writes scoped to jobs")
		if not doc.get("concurrency"):
			violations.append(f"{rel}: concurrency control is required")
		events = doc.get("on", doc.get(True, {}))
		if isinstance(events, dict) and "pull_request_target" in events:
			violations.append(f"{rel}: pull_request_target requires a separately reviewed policy exception")

		for job_name, job in jobs.items():
			if not isinstance(job, dict):
				violations.append(f"{rel}:{job_name}: job must be a mapping")
				continue
			if "uses" in job:
				check_uses(rel, job_name, job["uses"], violations)
			else:
				limit = job.get("timeout-minutes")
				if not isinstance(limit, int) or isinstance(limit, bool) or not 1 <= limit <= 360:
					violations.append(f"{rel}:{job_name}: explicit bounded timeout is required")

			for idx, step in enumerate(normalize_steps(job), start=1):
				if not isinstance(step, dict):
					continue
				step_name = str(step.get("name", f"{job_name}.step{idx}"))

				uses_value = step.get("uses")
				if isinstance(uses_value, str):
					check_uses(rel, step_name, uses_value.strip(), violations)

				run_value = step.get("run")
				if isinstance(run_value, str):
					check_run(rel, step_name, run_value, violations)

	return violations


def main() -> int:
	violations = audit(Path(__file__).resolve().parents[2])
	if violations:
		print("Workflow policy violations:")
		for v in violations:
			print(f"- {v}")
		return 1

	print("Workflow policy checks passed.")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
