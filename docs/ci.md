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
# CI and Sonar

This configuration targets the C17/CMake `best-of-best` development line
(Spine 1.3.0). The repository's current default branch is `develop`; its
older Autotools line has a separate CI stack. Do not replace that branch's
build with these CMake workflows without porting and validating it first.

The [workflow audit](ci-audit.md) records every inherited workflow, its
events, jobs, permissions, artifacts, concurrency and gaps. Existing native
correctness checks remain separate from Sonar:

- `CI`: GCC/Clang, default/small buffers, ASan/UBSan/leaks, platform tests,
  real SNMP/SQL polling, rejection, worker lifecycle and complete persistence.
- `Distro Matrix`: Linux distributions and native platform lanes; Tier1/2
  are blocking under the documented policy, Tier3 remains advisory.
- `Coverage`: instrumented production and CTest targets, LCOV/HTML/generic
  Sonar XML, and the unchanged 10% line gate.
- `Static Analysis`, `CodeQL`, `Security Posture`: existing C analyzers,
  lint, unsafe-API policy, secret scanning, workflow policy and security reports.
- `Integration`, `Fuzzing`, `Performance Regression`, nightly and weekly:
  distinct compatibility, runtime, sanitizer, performance and drift checks.
- `Release`, `Release Verification`, `OCI Publish`: existing packages,
  hardening, checksums, SBOMs, provenance and Sigstore signing.

PR correctness/security workflows now accept all target branches, including
stacked PR bases and best-of-best; inherited main/develop-only PR filters
no longer silently omit those checks. Push/schedule/release triggers remain
as documented in the audit.

Jobs have bounded timeouts (60 minutes for normal validation, 120 for
deep/VM lanes; Sonar45, policy5–10). Validation cancels obsolete runs for the
same PR/ref. Release/signing/publishing runs queue rather than canceling
an in-progress publication. Feature pushes and PRs can still produce two
runs; their different refs intentionally do not cancel each other. No
required merge check has been removed to eliminate that duplication.

## Selective analysis during modernization

`Sonar` always evaluates the branch policy and publishes a summary.
`Sonar analysis` runs only when `ENABLE_SONAR` is exactly `true` and
the event is eligible. Otherwise analysis is skipped. The stable
`Sonar Quality Gate` check reports this intentional skip as success in the
current optional mode, terminating cleanly rather than leaving a pending check.
Sonar is currently optional;
requested scanner, authentication, configuration and quality-gate errors
fail visibly, with no `continue-on-error`.

| Context | Analysis with ENABLE_SONAR=true |
| --- | --- |
| Push to the actual default branch | Run |
| Same-repository sonar/* or release/* PR | Run |
| Push to sonar/* or release/* | Run |
| Normal feat/feature/fix/refactor/chore/docs/experiment work | Skip |
| Manual dispatch, run_sonar=true | Run on selected branch |
| Manual dispatch, run_sonar=false | Skip |
| Fork or Dependabot PR | Skip; normal correctness CI still runs |
| ENABLE_SONAR absent or false | Skip |

The default branch is read from the GitHub event, not hard-coded. PR policy
uses the PR source branch; push policy uses `refs/heads/...`. Tags do not
request a branch analysis. Full checkout history retains SCM context.

## Maintainer setup and transition

Existing verified project: `https://sonarcloud.io`, organization
`somethingwithproof`, key `somethingwithproof_spine`. These existing
identifiers are in `sonar-project.properties`; no credentials are committed.
Required GitHub secret: `SONAR_TOKEN`, scoped to analysis of this project.
Required GitHub variable: `ENABLE_SONAR=true`. Leave `SONAR_REQUIRED` absent
or false during modernization. No host/project/organization
variables are needed for this existing Cloud deployment.

**Automatic Analysis is currently enabled in SonarCloud.** The workflow
switch controls only this Actions workflow; it cannot stop the external
automatic-analysis app. After merging and validating this workflow on a
supported branch, configure the GitHub secret and disable Automatic Analysis
in the project's SonarCloud Analysis Method settings before enabling CI
analysis. Leave the existing app active until that transition is ready.
No server setting or branch protection was changed by this PR.

GitHub requires a dispatchable workflow to exist on the repository's
default branch. Until this CMake configuration has been appropriately ported
to `develop`, manual dispatch is not available merely because the file is
on `best-of-best`. This branch limitation requires maintainer coordination;
do not change the repository default branch as a shortcut.

Once registered on the default branch, dispatch with:

```sh
gh workflow run sonar.yml --ref sonar/my-analysis -f run_sonar=true
```

Keep the workflow optional while modeling/architecture replacement is in
progress. A requested gate may fail on legitimate debt; do not suppress
rules or exclude source to make it pass. The preserved printf-style
variadic interfaces are an explicitly documented API decision.

## Coverage and local validation

The shared `collect-coverage` composite builds the actual `ci-main` preset
with GCC coverage and a compilation database, runs the native CTest suite,
and generates `coverage/sonar.xml`, `coverage.filtered.info` and HTML.
Both workflows use this same collection path. A separate Sonar build is
intentional: CFamily needs matching compiler context and generated headers
on the analysis runner; downloaded reports from an unrelated revision or
absolute build path are not a substitute.

`gcovr` reads unexecuted `.gcno` units as zero coverage, rather than reporting
only executed objects. First-party `src/` is included; tests, external system
headers and vendored files do not inflate production coverage. No first-party
coverage exclusions are added. Scripts remain analysis sources; this C
coverage report does not claim shell/Python coverage. The collector also runs the instrumented production CLI against the owned
repository SQL fixture in four profile/Boost combinations plus an invalid-output case, forcing batch
flushes and verifying exact producer bytes and scheduling. Its scenario,
binary, schema, runner and source hashes are required by the report validator.
The validator checks every compiled production unit against the report;
OS-guarded empty units require proof from the actual native gcov tool,
rather than a source exclusion. Missing/malformed analysis-policy outputs
and omitted worker coverage are regression-tested failures.
Separate SNMP/rejection/lifecycle Docker suites retain their own verification;
their subprocess coverage is not claimed by this collector.

The initial CTest-only Linux baseline passed29/29 tests and measured 15.7% lines,
29.6% functions and 10.9% branches across its compiled production units.
The expanded macOS instrumented producer run covers41.9% lines,61.9%
functions and27.6% branches, with native producer provenance verified. These
platform-specific denominators differ. They are coverage gaps to improve,
not evidence of complete coverage. They
are a different branch/scope from the develop stack's narrower producer
coverage report and must not be compared as identical denominators.

Reporting dependencies are version/hash locked in
`.github/requirements-coverage.txt`; CI policy/flawfinder dependencies use
`.github/requirements-ci.txt`. Official Python setup caches downloads keyed
by these locks. Distro packages remain provided by their native package
managers. Compiled objects, credentials and cross-platform binaries are not
shared in a cache. Regenerate locks with `uv pip compile --generate-hashes`
under the selected Python runtime and retain the license notices.

Canonical local commands (select required runtimes using `mise`):

```sh
python3 -m pip install --require-hashes -r .github/requirements-ci.txt
python3 .github/scripts/check-workflow-policy.py
python3 .github/scripts/test-ci-policy.py
actionlint .github/workflows/*.yml
cmake --preset ci-main
cmake --build --preset ci-main
ctest --test-dir build --output-on-failure --no-tests=error
bash tests/integration/test_poll_submission.sh
bash tests/integration/test_poll_persistence.sh
bash scripts/test-distros.sh
```

CTest failures and empty discovery fail validation. Diagnostics remain
uploaded after failures. The existing advisory Semgrep/Scorecard/clang-tidy
and Tier3 policy remains explicit; Sonar errors are never converted to
success. Native SARIF support on PRs, including forks, is preserved.

## Secret and publication boundaries

Default workflow permissions are `contents: read`. SARIF upload permission
belongs only to analyzer jobs. Release attachments, attestations, GHCR and
OIDC signing retain their needed job-specific permissions; release jobs do
not receive unused package-write permission. No `pull_request_target` or
privileged workflow_run is introduced.

Fork and Dependabot PRs cannot enter the token-dependent Sonar job, even
if their source changes the policy script. Build/test commands do not receive
`SONAR_TOKEN`; it is limited to credential preflight and the SHA-pinned
official scanner. Same-repository PR authors are trusted repository writers.
Tokens are never printed. Fork builds/lint and security scans retain the
ordinary PR event's restricted token.

Troubleshooting: first check the policy summary and switch, then the
credential-preflight error, Automatic Analysis mode, compile database and
coverage artifacts. The official scanner verifies its downloaded binary's
signature. Scanner authentication/configuration and quality-gate failures
remain separate visible failures. A missing credential after an eligible
request fails early; it is not silently treated as an intentional skip.

## Future required gate

Recommended current required checks: CI policy tests, GCC/Clang builds/tests,
native sanitizer/integration checks, Tier1/2 distro checks and important
security checks. Native GitHub currently reports no required status contexts;
these recommendations do not claim protections already enforce them.

Once the architecture stabilizes, enable Sonar, disable Automatic Analysis,
and set `SONAR_REQUIRED=true`. This broadens analysis to every trusted PR
and branch push and makes the stable check reject skipped, failed or
cancelled analysis, including a disabled ENABLE_SONAR switch. Require the
exact Actions check **Sonar Quality Gate**
in the applicable branch rule alongside all existing correctness gates.
Do not require a check name that exists only in a workflow absent from the
protected branch. No automatic branch-rule update is part of this change.

A skipped Actions job can satisfy a required check. Therefore simply adding
the check name while retaining optional policy does not enforce analysis.
The aggregate check runs even after a skipped/failed analysis and fails
closed in required mode. Fork/Dependabot analysis remains skipped safely;
its required-mode gate fails instead of allowing that skip to count as an
analysis. To use required mode with such contributions, a maintainer must
review and import the change onto a trusted same-repository branch while
preserving authorship, or establish another separately reviewed trusted
analysis path. Do not expose the token to fork code or use pull_request_target
to solve this. Review changes to the CI policy itself through the normal
independent-review process. Keep Sonar optional until those prerequisites
and current-head analyses are verified.

## Locked security scanner

The Scorecard job selects Go 1.26.8 through the pinned mise Action and builds
the existing Scorecard v5.5.0 root command using `.github/scorecard/go.mod`
and `go.sum` with `-mod=readonly`. The scanner module is CI tooling only;
Spine's native runtime and distro-managed dependencies are unchanged. Python
CI/coverage installations retain their existing hashes and accept wheels only.
