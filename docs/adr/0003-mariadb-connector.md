<!--
SPDX-FileCopyrightText: 2026 The Cacti Group
SPDX-License-Identifier: GPL-2.0-or-later
-->

# 0003: MariaDB Connector/C as the database client

## Status

Accepted

## Date

2026-10-07

## Context

Spine talks to the Cacti database through the MySQL C API. Two libraries
provide that API: MariaDB Connector/C (`libmariadb`, configured with
`mariadb_config`) and MySQL's `libmysqlclient` (`mysql_config`). `configure`
accepted either and took whichever it found first. CI built both.

Supporting both has costs that grow with the roadmap:

- Phase 4 moves polling onto libuv and needs a database client that does not
  block the event loop. Connector/C has a non-blocking API: each call has
  `_start` and `_cont` forms, and the library reports which socket event it
  is waiting for, so an external event loop can drive it. `libmysqlclient`
  has a different non-blocking API, added in MySQL 8.0.16. Supporting both
  would mean two event-loop integrations.
- The two libraries configure TLS differently. `sql.c` already sets options
  under `HAS_MYSQL_OPT_SSL_KEY` and `HAS_MYSQL_OPT_SSL_VERIFY_SERVER_CERT`,
  which `configure` probes because the libraries disagree. Phase 1 must prove
  database TLS and server identity checks end to end, and one library means
  one TLS code path to prove.
- Connector/C is the client distributions ship. Every lane in
  `scripts/test-distros.sh` and `.github/workflows/distro-matrix.yml` already
  builds against it, as do the Rocky Linux, Fedora, macOS, CodeQL, regression
  and nightly jobs. Only the `build` matrix and the jobs that used the
  `build-spine` action's default package built against `libmysqlclient`.
- Connector/C is licensed under the LGPL 2.1. `libmysqlclient` is GPL 2.0
  with MySQL's Universal FOSS Exception. Both can be combined with Spine's
  GPL, but the LGPL needs no exception to reason about when a packager links
  Spine statically or ships it with other software.

Connector/C speaks the MySQL protocol, including the `caching_sha2_password`
authentication that MySQL 8.4 uses by default, so choosing it does not drop
MySQL servers. The SNMPv3 integration job in `.github/workflows/integration.yml`
already runs Spine built against `libmariadb-dev` with a MySQL 8.4 server.

## Decision

MariaDB Connector/C is Spine's database client library.

- `configure` looks for `mariadb_config` and `libmariadb` first.
  `--with-mysql-client=mariadb` requires Connector/C and fails without it.
  `--with-mysql-client=mysql` selects `libmysqlclient`.
- `configure` decides which library it found by linking a call to
  `mariadb_get_infov()`, which only Connector/C provides. It does not trust
  the tool's name, because Debian and Homebrew install a `mysql_config` that
  belongs to Connector/C.
- When `libmysqlclient` is linked, by choice or because it was the only
  client found, `configure` prints a warning that it is deprecated, once
  where the check runs and again at the end of its output.
- Both server families stay supported: MariaDB and MySQL servers, reached
  through Connector/C. CI keeps testing against both.

Timeline:

1. The first release that includes this change (`develop` is versioned
   1.3.0) builds against `libmysqlclient` with a warning. CI keeps its
   `libmysqlclient` lanes, labelled deprecated.
2. The release after it removes `libmysqlclient` support:
   `--with-mysql-client=mysql`, the fallback to `mysql_config` and
   `libmysqlclient`, and the deprecated CI lanes.

## Consequences

Packagers who build against `libmysqlclient` see a warning now and must
switch to Connector/C within one release. On most distributions that is a
change of build dependency only, for example `libmariadb-dev` in place of
`libmysqlclient-dev`.

Phase 4 can use Connector/C's non-blocking API without a second
implementation, and the Phase 1 TLS work tests one library.

Spine still depends on Connector/C behaving like the MySQL client against
MySQL servers. A protocol or authentication change in a new MySQL release
reaches Spine only after Connector/C supports it. CI's MySQL server lanes
are what catch that.

Removing `libmysqlclient` deletes code paths that exist only for it, such as
probes that differ between the two libraries. That work belongs to the
removal release, not this one.
