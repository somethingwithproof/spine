# OpenSSF assessment evidence

Prepared on 2026-10-08 for `https://github.com/somethingwithproof/spine`.
This is an evidence worksheet, not a published assessment or certification.
Source changes cited from PR #38 are pending merge into `develop`.

The [OpenSSF program page](https://openssf.org/projects/best-practices-badge/)
links to the assessment service. A project-specific assessment must be created
or updated at [bestpractices.dev](https://www.bestpractices.dev/) by an
authenticated project owner. No project ID has been supplied for this fork.
Use this fork's repository URL, not upstream Cacti's identity or badge.

## Evidence available for the owner

The criterion identifiers below refer to the
[passing criteria](https://www.bestpractices.dev/en/criteria/0).
They identify evidence to examine, not answers already submitted.

| Area / criterion identifiers | Repository evidence |
| --- | --- |
| Description, participation and license: `description_good`, `interact`, `contribution`, `contribution_requirements`, `floss_license`, `floss_license_osi`, `license_location` | [README](../../README.md), [CONTRIBUTING](../../CONTRIBUTING.md), [LICENSE](../../LICENSE). The contribution guide describes PRs against develop, test expectations and coding conventions. |
| Interfaces: `documentation_basics`, `documentation_interface` | [Manual](../../spine.1), [configuration sample](../../spine.conf.dist), README and [module architecture](../architecture/source-modules.md). |
| Hosting and change tracking: `sites_https`, `discussion`, `english`, `maintained`, `repo_public`, `repo_track`, `repo_interim`, `repo_distributed`, `delivery_mitm` | Public HTTPS GitHub repository, issues and pull requests; Git history records authors and dates. PR #38 exposes interim changes before release. |
| Reporting: `report_process`, `report_tracker`, `report_archive`, `vulnerability_report_process`, `vulnerability_report_private` | GitHub issue templates and [SECURITY](../../SECURITY.md). GitHub's private vulnerability reporting setting was verified enabled on 2026-10-08. |
| Build and tests: `build`, `build_common_tools`, `build_floss_tools`, `test`, `test_invocation`, `test_continuous_integration`, `test_policy`, `tests_are_added`, `tests_documented_added` | Autotools, `make check`, CONTRIBUTING and [.github/workflows/ci.yml](../../.github/workflows/ci.yml). PR #38 adds behavior, log-mode and PHP-slot regressions. Its BSD fix passed 18 local suites, with one skip and zero failures. |
| Diagnostics: `warnings`, `warnings_fixed`, `warnings_strict`, `static_analysis`, `static_analysis_common_vulnerabilities`, `static_analysis_often` | Warning-enabled builds; CodeQL, Semgrep, cppcheck and clang-tidy workflows. The clang-tidy baseline records existing diagnostics; it does not establish that every warning has been fixed. |
| Dynamic checks: `dynamic_analysis`, `dynamic_analysis_unsafe`, `dynamic_analysis_enable_assertions` | ASan/UBSan and ThreadSanitizer CI, assertion-based cmocka suites, [fuzz targets](../../tests/fuzz/Makefile), and nightly Valgrind jobs. Check actual workflow conclusions before claiming a successful run. |
| Secrets: `no_leaked_credentials`, `delivery_unsigned` | Hash-checked CI downloads and pinned dependencies; full-history TruffleHog workflow. Inspect its latest result and distinguish fixture credentials from live credentials. |

## Answers requiring further verification

- **Release history:** SECURITY states that this fork has not published a
  release. Upstream tags and ChangeLog do not establish a fork release policy.
  Confirm unique versions, release tags, human-written release notes and
  vulnerability references before answering release criteria.
- **Response history:** issue-response criteria need an audit of reports and
  enhancement requests in the applicable time window. Private vulnerability
  response times require owner confirmation; absence of public reports does
  not prove an absence of private reports.
- **Developer knowledge:** secure-design and common-error knowledge are
  personal attestations by a primary developer. Source code alone cannot
  establish them.
- **Cryptography:** [SNMP sessions](../../src/snmp/session.c) delegate to
  Net-SNMP and [database connections](../../src/database/connection.c) to the
  client library. Legacy SNMP algorithms remain configurable, and the sample
  database configuration does not enable TLS. Audit default algorithms,
  key lengths, transport verification, randomness and interoperability
  requirements before answering cryptography criteria. Do not mark them all
  inapplicable merely because Spine delegates implementation.
- **Outstanding findings:** reconcile public advisories and code-scanning
  alerts, their ages, severity and exploitability against the merged revision.
  PR fixes and successful scans do not establish that every known vulnerability
  is resolved. See the [scan review](2026-10-code-scanning-review.md).
- **Coverage:** the changed-line coverage gate is evidence for changed code;
  it does not prove coverage of every branch or interface.

## Independent review history

GitHub alert 3251 currently reports 5 approved changesets out of 30 and a
score of 1. The sampled develop PRs #35–#37 have no independent human
approvals; #36 has an automated approval. PR #38's author is
`somethingwithproof`, the sole listed repository collaborator on 2026-10-08.
Requesting that account as its own reviewer cannot provide independent review.

[Scorecard's Code-Review check](https://github.com/ossf/scorecard/blob/main/docs/checks.md#code-review)
examines recent changes and excludes bot reviews. Genuine human review of
future changes can improve the history as older changes leave the sample.
One approval does not erase the existing unreviewed history. Do not rewrite
commits, fabricate approval records or suppress the finding to claim success.
The owner's choice to retain the current merge policy remains in effect.

## Publishing and verification

1. The owner registers this repository or supplies its existing project ID.
2. Review the evidence and unresolved questions, then submit truthful answers
   through the owner's authenticated assessment account.
3. Add a README badge only after its project URL and actual status are public.
4. After the source fixes merge, inspect the next develop Scorecard analysis
   and alert states. An assessment and a human review are separate requirements;
   neither can substitute for the other.
