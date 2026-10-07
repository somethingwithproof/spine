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
"""Exercise the production branch policy, CLI outputs and workflow guardrails."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


def load(name):
    path = Path(__file__).with_name(name + ".py")
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


sonar = load("sonar-policy")
gate = load("sonar-gate")
coverage = load("validate-coverage")
workflow = load("check-workflow-policy")
cppcheck = load("compare-cppcheck")


class SonarPolicy(unittest.TestCase):
    def event(self, branch="sonar/native", fork=False):
        return {"repository": {"full_name": "owner/spine", "default_branch": "develop"},
                "pull_request": {"head": {"ref": branch, "repo": {"full_name": "fork/spine" if fork else "owner/spine"}}}}

    def test_execution_matrix(self):
        for branch, expected in [("feat/work", False), ("feature/work", False),
                                 ("fix/work", False), ("refactor/work", False),
                                 ("chore/work", False), ("docs/work", False),
                                 ("experiment/work", False), ("sonar/work", True),
                                 ("release/work", True)]:
            for event_name in ("pull_request", "push"):
                with self.subTest(branch=branch, event=event_name):
                    self.assertEqual(sonar.eligibility(event_name, self.event(branch),
                                     "refs/heads/" + branch, "true", "maintainer")[0], expected)
        self.assertTrue(sonar.eligibility("push", self.event(), "refs/heads/develop", "true", "maintainer")[0])
        # Default branch comes from the event; this is not hard-coded to develop/main.
        event = self.event(); event["repository"]["default_branch"] = "trunk"
        self.assertTrue(sonar.eligibility("push", event, "refs/heads/trunk", "true", "maintainer")[0])

    def test_manual_switch_and_untrusted_inputs(self):
        self.assertTrue(sonar.eligibility("workflow_dispatch", self.event(), "refs/heads/fix/work", "true", "maintainer")[0])
        event = self.event(); event["inputs"] = {"run_sonar": "false"}
        self.assertFalse(sonar.eligibility("workflow_dispatch", event, "", "true", "maintainer")[0])
        for enabled in ("", "false", "TRUE"):
            self.assertFalse(sonar.eligibility("pull_request", self.event(), "", enabled, "maintainer")[0])
        self.assertFalse(sonar.eligibility("pull_request", self.event(fork=True), "", "true", "maintainer")[0])
        self.assertFalse(sonar.eligibility("pull_request", self.event(), "", "true", "dependabot[bot]")[0])
        self.assertFalse(sonar.eligibility("pull_request_target", self.event(), "", "true", "maintainer")[0])
        self.assertFalse(sonar.eligibility("push", self.event(), "refs/tags/release/work", "true", "maintainer")[0])
        self.assertFalse(sonar.eligibility("pull_request", {}, "", "true", "maintainer")[0])

    def test_real_cli_reports_skip_without_secret(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "event.json").write_text(json.dumps(self.event(fork=True)))
            env = dict(os.environ, GITHUB_EVENT_NAME="pull_request", GITHUB_EVENT_PATH=str(root / "event.json"),
                       GITHUB_OUTPUT=str(root / "output"), GITHUB_STEP_SUMMARY=str(root / "summary"),
                       ENABLE_SONAR="true", GITHUB_ACTOR="maintainer")
            env.pop("SONAR_TOKEN", None)
            result = subprocess.run([sys.executable, str(Path(sonar.__file__))], env=env,
                                    capture_output=True, text=True, check=True)
            self.assertEqual((root / "output").read_text(), "eligible=false\n")
            self.assertIn("Fork PR", result.stdout)
            self.assertIn("skipped", (root / "summary").read_text())

    def test_required_mode_and_failure_semantics(self):
        self.assertTrue(sonar.eligibility("pull_request", self.event("fix/work"), "", "true", "maintainer", "true")[0])
        self.assertFalse(sonar.eligibility("pull_request", self.event(fork=True), "", "true", "maintainer", "true")[0])
        self.assertEqual(gate.result("success", "false", "skipped", "")[0], 0)
        self.assertEqual(gate.result("success", "true", "success", "true")[0], 0)
        for state in ("failure", "cancelled", "skipped"):
            self.assertEqual(gate.result("success", "true", state, "")[0], 1)
        self.assertEqual(gate.result("success", "false", "skipped", "true")[0], 1)
        self.assertEqual(gate.result("failure", "false", "skipped", "")[0], 1)
        for malformed in ("", "TRUE", "unknown"):
            self.assertEqual(gate.result("success", malformed, "skipped", "")[0], 1)


class WorkflowPolicy(unittest.TestCase):
    def test_complete_good_workflow_and_negative_mutations(self):
        import yaml
        good = {"on": {"pull_request": None}, "permissions": {"contents": "read"},
                "concurrency": {"group": "test", "cancel-in-progress": True},
                "jobs": {"test": {"runs-on": "ubuntu-latest", "timeout-minutes": 15,
                         "steps": [{"uses": "actions/checkout@" + "a" * 40}]}}}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); path = root / ".github/workflows/test.yml"
            path.parent.mkdir(parents=True)
            self.assertTrue(workflow.audit(root))
            path.write_text(yaml.safe_dump(good))
            self.assertEqual(workflow.audit(root), [])
            mutations = [lambda d: d.update(jobs={}),
                         lambda d: d["permissions"].update(contents="write"),
                         lambda d: d.pop("concurrency"),
                         lambda d: d["jobs"]["test"].pop("timeout-minutes"),
                         lambda d: d["jobs"]["test"]["steps"][0].update(uses="actions/checkout@main"),
                         lambda d: d.update(jobs={"reuse": {"uses": "owner/repo/.github/workflows/build.yml@main"}}),
                         lambda d: d["on"].update(pull_request_target=None)]
            for mutation in mutations:
                document = json.loads(json.dumps(good)); mutation(document)
                path.write_text(yaml.safe_dump(document))
                self.assertTrue(workflow.audit(root))


class CppcheckEvidence(unittest.TestCase):
    def test_locations_move_but_new_diagnostics_and_counts_fail(self):
        baseline = "src/poller.c:10:2: warning: original [original]\n"
        shifted = "src/poller.c:100:20: warning: original [original]\n"
        self.assertFalse(cppcheck.regressions(shifted, baseline))
        self.assertFalse(cppcheck.regressions(shifted + "src/sql.c:1:2: note: caller context\n", baseline))
        self.assertFalse(cppcheck.regressions(shifted + shifted, baseline))
        duplicate = shifted + "src/poller.c:101:20: warning: original [original]\n"
        self.assertEqual(sum(cppcheck.regressions(duplicate, baseline).values()), 1)
        for changed in ("src/sql.c:10:2: warning: original [original]",
                        "src/poller.c:10:2: error: original [original]",
                        "src/poller.c:10:2: warning: new failure [new]"):
            self.assertTrue(cppcheck.regressions(changed, baseline))
        with self.assertRaises(ValueError):
            cppcheck.regressions(shifted, "")

    def test_cli_fails_closed_on_missing_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            report, baseline, output = (root / name for name in ("report", "baseline", "output"))
            report.write_text("src/poller.c:10:2: error: failure [failure]\n")
            baseline.write_text("src/poller.c:20:2: warning: original [original]\n")
            command = [sys.executable, cppcheck.__file__, str(report), str(baseline), str(output)]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn("error: failure", output.read_text())
            baseline.unlink()
            self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)


class CoverageEvidence(unittest.TestCase):
    def test_missing_stale_or_unexecuted_producers_fail(self):
        import hashlib
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            paths = {"src/spine.c": "main", "src/poller.c": "poll",
                     "src/host_worker.c": "worker", "build/spine": "binary",
                     "build/compile_commands.json": json.dumps([{"file": str(root / source), "directory": str(root), "output": str(root / (source + ".o"))} for source in ("src/spine.c", "src/poller.c", "src/host_worker.c")]), "tests/snmpv3/db/init.sql": "schema",
                     ".github/scripts/coverage-native.py": "runner"}
            for name, content in paths.items():
                path = root / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_text(content)
            (root / "coverage").mkdir()
            xml = '<coverage version="1"><file path="src/spine.c"><lineToCover lineNumber="1" covered="true"/></file><file path="src/poller.c"><lineToCover lineNumber="1" covered="true"/></file><file path="src/host_worker.c"><lineToCover lineNumber="1" covered="false"/></file></coverage>'
            (root / "coverage/sonar.xml").write_text(xml)
            hashes = {name: hashlib.sha256(content.encode()).hexdigest() for name, content in paths.items()}
            manifest = {"producer": "coverage-native",
                        "cases": [f"NATIVE_COVERAGE_PROFILE_{p}_BOOST_{b}_PASS" for p in (1, 2) for b in (0, 1)] + ["NATIVE_COVERAGE_INVALID_SCRIPT_PASS"],
                        "sources": {name: value for name, value in hashes.items() if name.startswith("src/")},
                        "binary_sha256": hashes["build/spine"], "compile_commands_sha256": hashes["build/compile_commands.json"],
                        "schema_sha256": hashes["tests/snmpv3/db/init.sql"], "runner_sha256": hashes[".github/scripts/coverage-native.py"]}
            path = root / "coverage/producer.json"
            path.write_text(json.dumps(manifest)); coverage.validate(root)
            mutations = [lambda d: d["cases"].pop(), lambda d: d["sources"].pop("src/host_worker.c"),
                         lambda d: d.update(binary_sha256="stale"), lambda d: d.update(schema_sha256="stale"),
                         lambda d: d.update(runner_sha256="stale"), lambda d: d.update(compile_commands_sha256="stale")]
            for mutation in mutations:
                bad = json.loads(json.dumps(manifest)); mutation(bad); path.write_text(json.dumps(bad))
                with self.assertRaises(ValueError): coverage.validate(root)
            path.write_text(json.dumps(manifest))
            omitted = xml.replace('<file path="src/host_worker.c"><lineToCover lineNumber="1" covered="false"/></file>', '')
            (root / "coverage/sonar.xml").write_text(omitted)
            with self.assertRaises(ValueError): coverage.validate(root)
            (root / "coverage/sonar.xml").write_text(xml.replace('covered="true"', 'covered="false"'))
            with self.assertRaises(ValueError): coverage.validate(root)
            path.unlink()
            with self.assertRaises(FileNotFoundError): coverage.validate(root)


if __name__ == "__main__":
    unittest.main()
