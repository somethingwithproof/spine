<!--
Copyright (C) 2004-2026 The Cacti Group

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 2.1 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
License for more details.

You should have received a copy of the GNU Lesser General Public License
along with this library; if not, write to the Free Software Foundation,
Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
-->
# Existing CI audit

Audited inherited best-of-best stack at `e10f0802308dac3c6121720f189e60f70de4cfe5` before CI changes. All 14 workflow bodies, build/test configuration, Dockerfiles, dependencies, repository instructions and native GitHub rules were inspected. Default branch: `develop` (older Autotools line); this checkout is the C17/CMake line.

Every inherited third-party Action reference is a full commit SHA. No workflow uses pull_request_target or workflow_run. No dependency cache or explicit job timeout was present. CI lacked concurrency; other workflows canceled in-progress runs, including publication. See [CI policy](ci.md) for the resulting changes.

## ci.yml

Purpose: CI.

Triggers: `{'workflow_dispatch': None, 'push': {'branches': ['develop', 'feat/**', 'fix/**', 'issue-**', 'ci/**', 'refactor/**']}, 'pull_request': None}`.

Workflow permissions: `{'contents': 'read'}`. Concurrency: `None`.

| Job | Dependencies | Extra permissions | Advisory |
| --- | --- | --- | --- |
| flawfinder | independent | inherited | False |
| build-cmake-linux | independent | inherited | False |
| build-no-systemd | independent | inherited | False |
| build-cmake-linux-sanitizers | independent | inherited | False |
| build-windows | independent | inherited | False |
| build-macos | independent | inherited | False |
| build-freebsd | independent | inherited | False |

Named secrets: `[]`; repository variables: `[]`. Only names were inspected; no secret values were read.

Artifacts: `['flawfinder-report', 'spine-windows-x64', 'crash-dumps']`.

Finding: missing concurrency/timeouts; repeated feature push and PR runs. GCC/Clang/default/small/sanitizer/platform/SNMP checks are distinct correctness contracts and are retained. Flawfinder Python floated3.x; tool install lacked hash verification.

## codeql.yml

Purpose: CodeQL.

Triggers: `{'push': {'branches': ['main', 'develop']}, 'pull_request': {'branches': ['main', 'develop']}, 'workflow_dispatch': None, 'schedule': [{'cron': '0 6 * * 1'}]}`.

Workflow permissions: `{'contents': 'read', 'security-events': 'write', 'actions': 'read'}`. Concurrency: `{'group': '${{ github.workflow }}-${{ github.ref }}', 'cancel-in-progress': True}`.

| Job | Dependencies | Extra permissions | Advisory |
| --- | --- | --- | --- |
| analyze | independent | inherited | False |

Named secrets: `[]`; repository variables: `[]`. Only names were inspected; no secret values were read.

Artifacts: `[]`.

Finding: globally scoped SARIF/actions permissions and missing required libuv build dependency; older pinned CodeQL Actionv3.

## coverage.yml

Purpose: Coverage.

Triggers: `{'push': {'branches': ['main', 'develop']}, 'pull_request': {'branches': ['main', 'develop']}, 'workflow_dispatch': None}`.

Workflow permissions: `{'contents': 'read'}`. Concurrency: `{'group': '${{ github.workflow }}-${{ github.ref }}', 'cancel-in-progress': True}`.

| Job | Dependencies | Extra permissions | Advisory |
| --- | --- | --- | --- |
| gcc-coverage | independent | inherited | False |

Named secrets: `[]`; repository variables: `[]`. Only names were inspected; no secret values were read.

Artifacts: `['coverage-report']`.

Finding: test failure swallowed, report capture only executed objects and ignored mismatches; missing data produced placeholder HTML. Existing line gate10% is preserved. Shared collector now reports unexecuted production units and native Sonar XML.

## distro-matrix.yml

Purpose: Distro Matrix.

Triggers: `{'workflow_dispatch': None, 'push': {'branches': ['develop', 'feat/**', 'fix/**', 'ci/**']}, 'pull_request': None, 'schedule': [{'cron': '17 6 * * 1'}]}`.

Workflow permissions: `{'contents': 'read'}`. Concurrency: `{'group': 'distro-matrix-${{ github.ref }}', 'cancel-in-progress': True}`.

| Job | Dependencies | Extra permissions | Advisory |
| --- | --- | --- | --- |
| linux | independent | inherited | ${{ matrix.tier >= 3 }} |
| macos | independent | inherited | False |
| freebsd | independent | inherited | False |
| netbsd | independent | inherited | True |
| openbsd | independent | inherited | True |
| windows | independent | inherited | True |

Named secrets: `[]`; repository variables: `[]`. Only names were inspected; no secret values were read.

Artifacts: `[]`.

## fuzzing.yml

Purpose: Fuzzing.

Triggers: `{'pull_request': {'branches': ['main', 'develop']}, 'workflow_dispatch': None, 'schedule': [{'cron': '0 6 * * 2'}]}`.

Workflow permissions: `{'contents': 'read'}`. Concurrency: `{'group': '${{ github.workflow }}-${{ github.ref }}', 'cancel-in-progress': True}`.

| Job | Dependencies | Extra permissions | Advisory |
| --- | --- | --- | --- |
| cli-fuzz-smoke | independent | inherited | False |

Named secrets: `[]`; repository variables: `[]`. Only names were inspected; no secret values were read.

Artifacts: `['fuzzing-artifacts']`.

## integration.yml

Purpose: Integration.

Triggers: `{'push': {'branches': ['main', 'develop']}, 'pull_request': {'branches': ['main', 'develop']}, 'workflow_dispatch': None}`.

Workflow permissions: `{'contents': 'read'}`. Concurrency: `{'group': '${{ github.workflow }}-${{ github.ref }}', 'cancel-in-progress': True}`.

| Job | Dependencies | Extra permissions | Advisory |
| --- | --- | --- | --- |
| db-integration | independent | inherited | False |
| netsnmp-compat | independent | inherited | False |
| docker-tests | independent | inherited | False |

Named secrets: `[]`; repository variables: `[]`. Only names were inspected; no secret values were read.

Artifacts: `['integration-${{ matrix.db_name }}-${{ matrix.db_version }}-logs', 'netsnmp-${{ matrix.snmp_version }}-log']`.

Finding: CTest failure swallowed; DB service lane did not prove schema-backed polling because the CTests do not consume SPINE_DB_* variables. Real polling remains in standalone Docker fixtures. Missing runtime build dependencies and misleading distro-based Net-SNMP version labels remain observable through package output.

## nightly.yml

Purpose: Nightly Heavy Checks.

Triggers: `{'schedule': [{'cron': '30 2 * * *'}], 'workflow_dispatch': None}`.

Workflow permissions: `{'contents': 'read'}`. Concurrency: `{'group': '${{ github.workflow }}-${{ github.ref }}', 'cancel-in-progress': True}`.

| Job | Dependencies | Extra permissions | Advisory |
| --- | --- | --- | --- |
| tsan | independent | inherited | False |
| asan-soak | independent | inherited | False |
| valgrind | independent | inherited | False |
| fuzz-smoke | independent | inherited | False |
| helgrind | independent | inherited | False |
| stack-usage | independent | inherited | False |
| soak-placeholder | ['tsan', 'asan-soak', 'valgrind', 'fuzz-smoke', 'helgrind', 'stack-usage'] | inherited | False |

Named secrets: `[]`; repository variables: `[]`. Only names were inspected; no secret values were read.

Artifacts: `['nightly-tsan-logs', 'nightly-asan-logs', 'nightly-valgrind-logs', 'nightly-fuzz-logs', 'nightly-helgrind-logs', 'stack-usage']`.

Finding: swallowed CTest/fallback failures, missing libuv and placeholder fuzz/soak/Helgrind paths. Preserve leak baselines and advisory distinctions; placeholders do not establish executed coverage.

## oci.yml

Purpose: OCI Publish.

Triggers: `{'push': {'branches': ['develop', 'main'], 'tags': ['v*']}, 'workflow_dispatch': None}`.

Workflow permissions: `{'contents': 'read'}`. Concurrency: `{'group': '${{ github.workflow }}-${{ github.ref }}', 'cancel-in-progress': True}`.

| Job | Dependencies | Extra permissions | Advisory |
| --- | --- | --- | --- |
| publish-oci | independent | {'contents': 'read', 'packages': 'write', 'id-token': 'write'} | False |

Named secrets: `['GITHUB_TOKEN']`; repository variables: `[]`. Only names were inspected; no secret values were read.

Artifacts: `[]`.

Finding: publication concurrency canceled in-progress signing/publish operations; build dependencies lacked libuv in native release lanes. Preserve SBOM/checksum/signature/provenance steps and scope writes to their owning job. OCI metadata inherited an upstream-owned image namespace.

## perf-regression.yml

Purpose: Performance Regression.

Triggers: `{'pull_request': {'branches': ['main', 'develop']}, 'workflow_dispatch': None, 'schedule': [{'cron': '0 7 * * 1'}]}`.

Workflow permissions: `{'contents': 'read'}`. Concurrency: `{'group': '${{ github.workflow }}-${{ github.ref }}', 'cancel-in-progress': True}`.

| Job | Dependencies | Extra permissions | Advisory |
| --- | --- | --- | --- |
| cli-benchmark | independent | inherited | False |
| snmp-simulator-benchmark | independent | inherited | False |
| poll-benchmark | independent | inherited | False |

Named secrets: `[]`; repository variables: `[]`. Only names were inspected; no secret values were read.

Artifacts: `['perf-cli-results', 'perf-snmp-results', 'poll-benchmark']`.

## release-verification.yml

Purpose: Release Verification.

Triggers: `{'workflow_dispatch': None, 'push': {'tags': ['v*', 'release-*']}}`.

Workflow permissions: `{'contents': 'read'}`. Concurrency: `{'group': '${{ github.workflow }}-${{ github.ref }}', 'cancel-in-progress': True}`.

| Job | Dependencies | Extra permissions | Advisory |
| --- | --- | --- | --- |
| release-verify | independent | {'contents': 'read', 'id-token': 'write', 'attestations': 'write'} | False |

Named secrets: `[]`; repository variables: `[]`. Only names were inspected; no secret values were read.

Artifacts: `['release-verification']`.

Finding: publication concurrency canceled in-progress signing/publish operations; build dependencies lacked libuv in native release lanes. Preserve SBOM/checksum/signature/provenance steps and scope writes to their owning job. OCI metadata inherited an upstream-owned image namespace.

## release.yml

Purpose: Release.

Triggers: `{'push': {'tags': ['v*']}, 'workflow_dispatch': None}`.

Workflow permissions: `{'contents': 'read'}`. Concurrency: `{'group': '${{ github.workflow }}-${{ github.ref }}', 'cancel-in-progress': True}`.

| Job | Dependencies | Extra permissions | Advisory |
| --- | --- | --- | --- |
| build-and-sign | independent | {'contents': 'write', 'id-token': 'write', 'packages': 'write'} | False |

Named secrets: `[]`; repository variables: `[]`. Only names were inspected; no secret values were read.

Artifacts: `['release-artifacts']`.

Finding: publication concurrency canceled in-progress signing/publish operations; build dependencies lacked libuv in native release lanes. Preserve SBOM/checksum/signature/provenance steps and scope writes to their owning job. OCI metadata inherited an upstream-owned image namespace.

## security-posture.yml

Purpose: Security Posture.

Triggers: `{'push': {'branches': ['main', 'develop']}, 'pull_request': {'branches': ['main', 'develop']}, 'workflow_dispatch': None, 'schedule': [{'cron': '0 5 * * 1'}]}`.

Workflow permissions: `{'contents': 'read', 'security-events': 'write'}`. Concurrency: `{'group': '${{ github.workflow }}-${{ github.ref }}', 'cancel-in-progress': True}`.

| Job | Dependencies | Extra permissions | Advisory |
| --- | --- | --- | --- |
| trufflehog | independent | inherited | False |
| semgrep | independent | inherited | True |
| scorecard | independent | inherited | True |
| workflow-policy | independent | inherited | False |

Named secrets: `['GITHUB_TOKEN']`; repository variables: `[]`. Only names were inspected; no secret values were read.

Artifacts: `['semgrep-report', 'scorecard-report']`.

Finding: global SARIF permission; Scorecard install used@latest. Existing Semgrep/Scorecard are explicitly advisory. Workflow policy missed malformed/empty job maps, reusable-job refs, privilege and timeout policy.

## static-analysis.yml

Purpose: Static Analysis.

Triggers: `{'push': {'branches': ['main', 'develop']}, 'pull_request': {'branches': ['main', 'develop']}, 'workflow_dispatch': None}`.

Workflow permissions: `{'contents': 'read', 'security-events': 'write'}`. Concurrency: `{'group': '${{ github.workflow }}-${{ github.ref }}', 'cancel-in-progress': True}`.

| Job | Dependencies | Extra permissions | Advisory |
| --- | --- | --- | --- |
| actionlint | independent | inherited | False |
| shell-lint | independent | inherited | False |
| codespell | independent | inherited | False |
| clang-tidy | independent | inherited | True |
| scan-build | independent | inherited | False |
| cppcheck | independent | inherited | False |

Named secrets: `[]`; repository variables: `[]`. Only names were inspected; no secret values were read.

Artifacts: `['codespell-report', 'clang-tidy-report', 'scan-build-report', 'cppcheck-report']`.

Finding: globally scoped SARIF permission; clang-tidy override flags discarded compile-database context and failed source discovery could look empty. Fix discovery failures without adding analyzer baseline entries.

## weekly.yml

Purpose: Weekly Deep Checks.

Triggers: `{'schedule': [{'cron': '0 4 * * 0'}], 'workflow_dispatch': None}`.

Workflow permissions: `{'contents': 'read'}`. Concurrency: `{'group': '${{ github.workflow }}-${{ github.ref }}', 'cancel-in-progress': True}`.

| Job | Dependencies | Extra permissions | Advisory |
| --- | --- | --- | --- |
| reproducible-build | independent | inherited | False |
| include-graph | independent | inherited | False |
| license-check | independent | inherited | False |
| spell-check | independent | inherited | False |

Named secrets: `[]`; repository variables: `[]`. Only names were inspected; no secret values were read.

Artifacts: `['include-graph']`.

Finding: package/tool versions float; reproducibility and spelling failures are advisory by existing design; missing libuv prevents useful native builds.
