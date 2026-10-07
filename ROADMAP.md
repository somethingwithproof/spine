<!--
SPDX-FileCopyrightText: 2026 The Cacti Group
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Spine roadmap

This roadmap covers October 2026 to early 2028. It aims to make Spine a
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
  record the decision as an ADR. The CMake move in Phase 2 and the poller
  rewrite in Phase 4 both mean upstream changes must be ported by hand.

Done when: the gate passes, required checks block red merges, and the ADR is
merged.

## Phase 1: reliability and configuration (Q1 2027)

- Validate configuration at startup with `--check`, and print the effective
  configuration with secrets redacted.
- Read credentials from a file or environment variable, never from the
  command line.
- Make MariaDB Connector/C the only database client library. It has the
  non-blocking API Phase 4 needs, it is the client most distributions ship,
  and one library means one TLS code path. Deprecate `libmysqlclient` builds
  for one release, then remove them. Keep testing against both server
  families in CI: MariaDB 10.11 and 11.x, and MySQL 8.4 and 9.x, including
  `caching_sha2_password` authentication.
- Prove database TLS end to end, including server identity checks, against
  TLS-enabled MariaDB and MySQL servers in CI.
- Give each device a time budget so one slow device cannot overrun the
  polling interval.
- Run a nightly soak test and track memory over time.
- Move the build from C99 to C17, recorded in an ADR. Adopt features one
  area at a time, each with tests: `<stdatomic.h>` for shared counters now
  guarded by global locks, and `_Static_assert` for buffer and struct sizes.
  Keep pthreads; `<threads.h>` is missing on macOS. Upstream C99 code still
  compiles as C17, so the switch does not block upstream merges.
- Annotate locks and the data they protect for Clang's thread-safety
  analysis (`-Wthread-safety`), so the compiler rejects unlocked access to
  shared state.
- Test SNMPv3 with SHA-2 authentication and AES-192 and AES-256 privacy
  against a live agent, and log a deprecation warning for MD5 and DES.
- Size worker threads from the CPU quota the process actually has (cgroup v2
  `cpu.max` and CPU affinity), not the host's core count.
- Accept credentials from systemd `LoadCredential`, and document a path for
  HashiCorp Vault and cloud secret managers.
- Build with a 64-bit `time_t` everywhere (`_TIME_BITS=64` on 32-bit
  targets), or declare 32-bit platforms unsupported.
- Add a `clang-format` configuration and pre-commit hooks so tools, not
  reviewers, enforce style.
- Add CI lanes for the newest GCC and Clang. Newer compilers turn unsafe
  legacy behaviour into errors, and GCC 15 defaults to C23, so every build
  passes an explicit `-std`. Revisit C23 once the oldest supported
  distribution can build it; Rocky Linux 8 ships GCC 8 and is supported
  until May 2029.

Done when: the fault suite covers a database outage, an SNMP timeout storm,
a hung script and a crashed PHP script server, and asserts the outcome of
each; a 24-hour soak shows no memory growth; and overall line coverage is
at least 80%.

## Phase 2: observability and build system (Q1 to Q2 2027)

- Emit structured JSON logs with stable keys and a per-cycle run ID.
- Record per-cycle metrics: devices polled and failed, poll latency, SNMP
  errors by class, and script timeouts. Spine exits after every cycle, so it
  pushes metrics at the end of the run: a Prometheus textfile and OTLP.
- Offer an optional OpenTelemetry span for each device poll.
- Replace autotools with CMake and Ninja, using presets for the CI builds.
  Port every feature probe and `AC_DEFINE`. Until autotools is removed, a CI
  job builds both ways and fails if the generated `config.h` files differ;
  the retired best-of-best CMake port lost its MySQL TLS, retry and Solaris
  defines without anyone noticing. Keep the install paths, the man page and
  a source tarball for packagers, run the tests through `ctest`, and feed
  the native `compile_commands.json` to SonarCloud and clang-tidy.

Done when: an operator can see which devices failed in a cycle, and why,
without reading log files; and CMake builds and tests every lane in the
support matrix with a `config.h` identical to the autotools one, after which
autotools is removed.

## Phase 3: security depth (Q2 to Q3 2027)

- Write a threat model covering the trust boundaries: the Cacti database,
  scripts, the network and the setuid start.
- Decide, in the threat model, whether to split Spine the way OpenSSH
  separates privileges. Today one process parses replies from untrusted
  devices and script output, and also holds the Cacti database credentials.
  A split would run sandboxed collector processes that poll and parse, with
  no database access, and a coordinator that validates their results and
  alone writes to the database. A parser bug would then reach bad values,
  not the database.
- Publish a memory-safety roadmap, as CISA and NSA guidance asks of C
  projects: the GCC `-fhardened` flag set, `_FORTIFY_SOURCE=3`,
  `-fstrict-flex-arrays=3`, `-ftrivial-auto-var-init=zero`, bounds-checked
  string helpers throughout, and an evaluation of rewriting the parsers of
  untrusted input in Rust.
- Fuzz every parser: SNMP values, script output, configuration and host
  names.
- Ship SELinux and AppArmor profiles that allow script polling.
- Add a seccomp or Landlock sandbox only if the threat model calls for one.

Done when: each boundary in the threat model has a mitigation, fuzzers run
weekly, and the profiles pass on Rocky Linux and Ubuntu.

## Phase 4: event-driven poller on libuv (Q3 2027 to Q1 2028)

Phases 0 to 3 make the current threaded poller stable and correct first.
Spine polls one device per thread and blocks on every request. Phase 4
replaces that with an event-driven poller on libuv, so a few threads keep
thousands of requests in flight. The retired best-of-best line tried libuv
and failed: its poller ran the old blocking code on libuv's default pool of
four threads. This plan makes every I/O source non-blocking and tests that
none blocks a loop.

1. Baseline. Build a benchmark harness with simulated SNMP agents. Record
   throughput, poll latency, CPU and memory at 1,000, 10,000 and 50,000
   devices, and fail CI on regressions. Set the Phase 4 throughput target in
   an ADR from these numbers.
2. Loop design. Run one libuv loop per worker thread and shard devices
   across loops. Each device is a state machine: reachability, reindex
   checks, SNMP batches, scripts, then the result write. Root is already
   dropped before any loop starts.
   If Phase 3 chose the collector and coordinator split, the loops run in
   the sandboxed collector processes and only the coordinator talks to the
   database.
3. I/O sources:
   - SNMP: Net-SNMP's single-session API. Watch its sockets with `uv_poll`
     and drive its timeouts with `uv_timer`.
   - ICMP: the shared raw socket and the datagram sockets on `uv_poll`.
   - Scripts: `uv_spawn` in a process group, with pipes and timers.
   - PHP script server: `uv_pipe` with per-request deadlines.
   - Database: MariaDB Connector/C's non-blocking API on `uv_poll`.
4. Migration. Ship the event poller behind `--poller=event`, next to the
   threaded poller. Run the contract, fault and live suites against both in
   CI. Make it the default once it meets the exit test, and remove the
   threaded poller one release later.
5. Optional persistent mode. Cacti's `poller.php` starts Spine once per
   cycle, so Spine is not a long-running daemon today. Once the event poller
   is stable, offer a mode where Spine stays running: it keeps SNMP sessions
   and database connections open, schedules its own cycles, reports health
   through `sd_notify` with a watchdog, serves a live Prometheus endpoint,
   and accepts local control over Varlink. This needs a matching change in
   Cacti, and the per-cycle mode stays as the fallback.

Done when: the event poller passes every contract, fault and live test; a
test fails if any blocking call runs on a loop thread; TSan reports
nothing; memory stays bounded in the soak test; and throughput at 10,000
and 50,000 devices meets the target set in step 1.

## Phase 5: releases and supply chain (Q3 to Q4 2027)

The first releases ship the threaded poller from Phases 0 to 3. The event
poller joins a release only after it meets its Phase 4 exit test.

- Adopt semantic versioning, keep a changelog and maintain a long-term
  support branch.
- Sign release tarballs with cosign, publish SLSA provenance and a CycloneDX
  SBOM, and verify that builds reproduce.
- Build releases for generic CPUs, with compressed DWARF 5 debug
  information split into separate debug packages. Enable link-time
  optimisation only if the Phase 4 benchmark shows a gain.
- Publish a support matrix of distributions, MySQL and MariaDB versions,
  Net-SNMP versions and Cacti versions, each cell tested in CI. Support only
  releases still in vendor support: add a release after it ships, drop it at
  its end-of-life date, and let a weekly CI check flag any lane past end of
  life.
- Define a compatibility and deprecation policy for configuration keys and
  command-line options.
- Write down and version the contract with Cacti: exit codes, the
  `poller_output` format, the tables and columns Spine reads, and the
  command-line options. Test it in CI against each supported Cacti version.
- Build and release native arm64 binaries alongside x86-64.
- Publish an official multi-architecture container image that runs as a
  non-root user with only `CAP_NET_RAW`, signed like the tarballs.
- Handle vulnerabilities in the open: a disclosure policy, GitHub Security
  Advisories with CVE IDs, security fixes backported to the long-term
  support branch, and the OpenSSF Best Practices badge. This covers what the
  EU Cyber Resilience Act asks of software shipped commercially, with full
  obligations from December 2027.

Done when: a third party can verify the first signed release, and every
supported combination passes CI.

## Phase 6: operator documentation (throughout)

- An install and hardening guide, with a sandboxed systemd unit example for
  Cacti's poller (`ProtectSystem=strict`, `PrivateTmp`, a minimal
  capability set).
- A configuration reference.
- Runbooks for a database outage, SNMP timeouts, ICMP privileges and
  performance tuning.
- An architecture overview and an upgrade guide for each release.

## Non-goals

- In-process plugins loaded with `dlopen` or `libffi`. Cacti already extends
  polling through scripts and the PHP script server, which run in their own
  processes. Loading third-party code into Spine would put it next to the
  database credentials, the opposite of the Phase 3 collector split.
- Linux-only I/O such as io_uring. libuv already uses it where that helps.

## Risks and dependencies

- The event-driven poller rewrites `poller.c`, so upstream poller changes
  will no longer merge. From Phase 4 the fork keeps the Cacti database and
  output contract identical but ports upstream poller fixes by hand. The
  Phase 0 ADR must say so.
- The event-driven poller is the riskiest change. The threaded poller stays
  the default until the event poller meets its exit test.
- libuv becomes a build dependency. Every distribution in the support
  matrix must ship it; RHEL-family builds need CodeReady Builder or EPEL.
- With one maintainer, required checks and the test suite stand in for a
  second reviewer.
