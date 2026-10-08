<!--
SPDX-FileCopyrightText: 2026 The Cacti Group
SPDX-License-Identifier: GPL-2.0-or-later
-->

# 0002: Upstream tracking

## Status

Accepted

## Date

2026-10-07

## Context

This repository is a fork of Cacti's Spine. Until now `develop` has been
upstream `develop` plus the fork's own commits, rebased so that upstream
history stays an ancestor. Each sync replays upstream's commits; in October
2026 about a third of 58 upstream commits touched the build files, and the
fork's larger changes made many of them conflict.

The roadmap commits the fork to two changes upstream does not share. Phase 2
replaces autotools with CMake, so upstream changes to `configure.ac` and
`Makefile.am` will no longer apply. Phase 4 replaces the threaded poller with
an event-driven poller on libuv, so upstream changes to `poller.c` will no
longer apply either. Both are needed for the roadmap's goals: a portable
build, Windows support and a poller that keeps many requests in flight.

Spine's contract with Cacti is the database schema it reads, the
`poller_output` format it writes, its exit codes and its command-line
options. Cacti users depend on that contract, not on Spine's internals.

## Decision

The fork stops rebasing `develop` onto upstream once the Phase 2 CMake move
merges. Until then it keeps syncing as it does now.

After that point, upstream changes are reviewed and ported by hand. Security
fixes are ported first, then correctness fixes, then features that fit the
roadmap. Each ported change cites the upstream commit in its message
(`git cherry-pick -x` where the code still applies).

The contract with Cacti stays compatible with the Cacti versions in the
support matrix. Phase 5 writes that contract down and tests it in CI.

## Consequences

Upstream fixes stop arriving for free. Someone has to watch upstream and
port what matters, which is ongoing work for a single maintainer.

In return, the fork can change its build and its poller without carrying
upstream's layout, and syncs no longer stall on build-file conflicts.

Fixes made here are not offered upstream, so upstream will not benefit
from them.

If the fork later wants to rejoin upstream, this record is superseded and
the CMake and poller work would have to be offered upstream or reverted.
