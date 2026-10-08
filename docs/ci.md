<!--
SPDX-FileCopyrightText: 2026 The Cacti Group
SPDX-License-Identifier: GPL-2.0-or-later
-->

# CI

## SonarCloud analysis

`.github/workflows/sonar.yml` analyzes Spine in the existing SonarCloud
project `somethingwithproof_spine` (organization `somethingwithproof`). The
identifiers live in `sonar-project.properties`; no credential is committed.

The workflow is off until a maintainer enables it. The `Sonar execution
policy` job always runs and writes its decision to the job summary. While
`ENABLE_SONAR` is unset, `Sonar analysis` is skipped and `Sonar Quality Gate`
reports the skip as success.

With `ENABLE_SONAR=true`, analysis runs for pushes to the default branch
(`develop`), pushes and same-repository PRs from `sonar/*` or `release/*`
branches, and manual dispatch. Fork and Dependabot PRs never reach the job
that holds `SONAR_TOKEN`. Setting `SONAR_REQUIRED=true` widens analysis to
every trusted branch and PR and makes the gate fail when analysis is skipped.

The analysis job runs on the same runner as the build so the scanner sees the
real compiler context:

1. `bear` records `compile_commands.json` from `make check`. The step fails if
   any production or test source has no entry.
2. `tests/tools/run_coverage.sh` rebuilds with `--coverage` and runs the same
   regression set as `regressions.yml`, against a MariaDB service and a local
   `snmpd`.
3. `gcovr`, pinned with hashes in `.github/requirements-coverage.txt`, turns
   the profiles into `coverage/sonar.xml`.
4. The SonarSource scanner uploads the analysis and waits for the quality
   gate.

Each analysis names its branch context. A push or dispatch on the default
branch reports to the SonarCloud main branch. Any other branch passes
`sonar.branch.name`, and a PR passes the `sonar.pullrequest.*` keys, so a
side-branch run never replaces the `develop` analysis. Tag refs and ref names
outside `[A-Za-z0-9._/-]` are rejected before the build starts.

### Enabling analysis

1. In SonarCloud, create a token under My Account > Security. A project
   analysis token for `somethingwithproof_spine` is enough.
2. In GitHub, add it as the repository secret `SONAR_TOKEN`
   (Settings > Secrets and variables > Actions > Secrets).
3. In SonarCloud, turn off Automatic Analysis under Administration >
   Analysis Method. CI analysis fails while Automatic Analysis is on. For
   `somethingwithproof_spine` this is already done.
4. In GitHub, add the repository variable `ENABLE_SONAR` with the value
   `true` (Settings > Secrets and variables > Actions > Variables).
5. Push to `develop` or run the workflow by hand:
   `gh workflow run sonar.yml --ref develop -f run_sonar=true`.

If `ENABLE_SONAR` is `true` and the secret is missing, the job stops at
`Verify analysis credential` with an error rather than skipping. To turn
analysis off again, unset `ENABLE_SONAR`; re-enable Automatic Analysis in
SonarCloud only if CI analysis is no longer wanted.

## Security scanning

`.github/workflows/security-scanning.yml` runs three jobs. Every action is
pinned to a commit, and every downloaded tool to a digest or checksum.

- `OpenSSF Scorecard` runs on pushes to `develop` and on the weekly
  schedule. It publishes results to the OpenSSF API and uploads SARIF to
  code scanning. The API rejects results unless the workflow has no
  top-level `env` or `defaults`, no workflow-level write permission, and
  `id-token: write` only on the Scorecard job, and unless that job uses only
  its allowed actions. Keep those rules when editing the file.
- `Secret scan (full history)` checks out every branch and tag and runs
  TruffleHog over all commits. The release tarball is checked against a
  pinned SHA-256. Only secrets that TruffleHog verifies as live fail the
  job.
- `Semgrep` runs the `p/c` and `p/secrets` rule sets in the official
  `semgrep/semgrep` image, pinned by digest, and uploads SARIF to code
  scanning. Any finding fails the job. The rule sets are fetched from the
  Semgrep registry at run time, so new rules can appear without a change
  here.

To update TruffleHog, take the new SHA-256 from the release's
`checksums.txt` after verifying that file's cosign signature. To update
Semgrep, pin the new image index digest
(`docker buildx imagetools inspect semgrep/semgrep:<version>`).
