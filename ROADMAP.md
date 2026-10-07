<!--
SPDX-FileCopyrightText: 2026 The Cacti Group
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Spine roadmap

This roadmap covers October 2026 to December 2027. It aims to make Spine a
poller that an operator can run, audit and upgrade with confidence at large
scale. Each phase states what "done" means as a test, not a feeling. Dates
are targets, not commitments.

Status as of 2026-10-07.

## Current state

| Area | State |
| --- | --- |
| Privilege model | A setuid Spine drops root permanently before it reads any input, and verifies the drop (#23). |
| Failure isolation | A database or script failure fails one device, not the process (#18). |
| Data correctness | One classifier decides how poll output is stored; all SNMP batch flushes share one path (#21). |
| Reachability | Per-attempt ICMP deadlines, IPv6 host names, bounded command-line parsing (#19). |
| Secrets in logs | Script and script-server commands are never logged in full (#18). |
| CI | gcc and clang builds, ASan, UBSan, TSan, cppcheck, changed-line coverage, CodeQL and SonarCloud (#20, #22). |

Known gaps:

- The SonarCloud quality gate fails on `develop`.
- Spine has no metrics and no structured telemetry.
- SNMP requests block: one thread polls one device at a time.
- There is no release process, signing or SBOM.
- No support matrix says which distributions and library versions work.
- Overall line coverage is 69.5%.
- Branch protection on `develop` requires no status checks.
- There is no operator documentation beyond `INSTALL` and `README.md`.

## Phase 0: finish the base (Q4 2026)

- Fix or formally accept every SonarCloud finding until the gate passes.
- Port the useful parts of the retired best-of-best line (tag
  `archive/best-of-best`): the `snmp_sess_error` fix, compiler hardening
  flags, a distribution and BSD build matrix, static analysis and nightly
  sanitizer jobs, secret handling for logs and core files, and the `--check`
  and `--dump-config` options.
- Add Scorecard, secret scanning and semgrep to CI.
- Add `SECURITY.md` and `CONTRIBUTING.md`.
- Make `CI / required` and the SonarCloud gate required checks on `develop`.
- Decide whether this fork keeps tracking upstream or diverges for good, and
  record the decision as an ADR. The decision sets the build system: Spine
  stays on autotools while it tracks upstream.

Done when: the gate passes, required checks block red merges, and the ADR is
merged.

## Phase 1: reliability and configuration (Q1 2027)

- Validate configuration at startup with `--check`, and print the effective
  configuration with secrets redacted.
- Read credentials from a file or environment variable, never from the
  command line.
- Prove database TLS end to end, including server identity checks, against a
  TLS-enabled MariaDB in CI.
- Give each device a time budget so one slow device cannot overrun the
  polling interval.
- Run a nightly soak test and track memory over time.

Done when: the fault suite covers a database outage, an SNMP timeout storm,
a hung script and a crashed PHP script server, and asserts the outcome of
each; a 24-hour soak shows no memory growth.

## Phase 2: observability (Q1 to Q2 2027)

- Emit structured JSON logs with stable keys and a per-cycle run ID.
- Record per-cycle metrics: devices polled and failed, poll latency, SNMP
  errors by class, and script timeouts. Spine exits after every cycle, so it
  pushes metrics at the end of the run: a Prometheus textfile and OTLP.
- Offer an optional OpenTelemetry span for each device poll.

Done when: an operator can see which devices failed in a cycle, and why,
without reading log files.

## Phase 3: performance and scale (Q2 to Q3 2027)

- Build a benchmark harness with simulated SNMP agents, record a baseline,
  and fail CI on regressions.
- Use the Net-SNMP asynchronous API so each worker keeps many requests in
  flight.
- Batch database writes and remove allocation from the per-device loop.

Done when: measured throughput at 10,000 and 50,000 devices beats the
baseline. If asynchronous SNMP does not gain more than it risks, it is
withdrawn.

## Phase 4: security depth (Q3 2027)

- Write a threat model covering the trust boundaries: the Cacti database,
  scripts, the network and the setuid start.
- Fuzz every parser: SNMP values, script output, configuration and host
  names.
- Ship SELinux and AppArmor profiles that allow script polling.
- Add a seccomp or Landlock sandbox only if the threat model calls for one.

Done when: each boundary in the threat model has a mitigation, fuzzers run
weekly, and the profiles pass on Rocky Linux and Ubuntu.

## Phase 5: releases and supply chain (Q3 to Q4 2027)

- Adopt semantic versioning, keep a changelog and maintain a long-term
  support branch.
- Sign release tarballs with cosign, publish SLSA provenance and a CycloneDX
  SBOM, and verify that builds reproduce.
- Publish a support matrix of distributions, MySQL and MariaDB versions,
  Net-SNMP versions and Cacti versions, each cell tested in CI.
- Define a compatibility and deprecation policy for configuration keys and
  command-line options.

Done when: a third party can verify the first signed release, and every
supported combination passes CI.

## Phase 6: operator documentation (throughout)

- An install and hardening guide.
- A configuration reference.
- Runbooks for a database outage, SNMP timeouts, ICMP privileges and
  performance tuning.
- An architecture overview and an upgrade guide for each release.

## Risks and dependencies

- The Phase 0 decision on tracking upstream shapes Phases 3 and 5. Diverging
  makes deep changes cheaper but ends free upstream fixes.
- Asynchronous SNMP is the riskiest change. It proceeds only on measured
  gains.
- With one maintainer, required checks and the test suite stand in for a
  second reviewer.
