<!--
SPDX-FileCopyrightText: 2026 The Cacti Group
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Platforms

`.github/workflows/distro-matrix.yml` builds Spine with Autotools and runs
`make check` on each platform below, on every pull request and push to
`develop` that touches the build or the sources.

## Support policy

A release is supported until its vendor end-of-life date. New releases are
added after general availability, and a lane is removed when its release
reaches end of life. Dates come from [endoflife.date](https://endoflife.date).
`.github/workflows/distro-eol.yml` runs `.github/scripts/check-distro-eol.py`
weekly and fails if any lane has passed end of life or is not listed. It
only reports; it opens no issue or pull request.

## Tested platforms

| Platform | Releases | How |
| --- | --- | --- |
| Rocky Linux | 8, 9, 10 | `rockylinux/rockylinux` container |
| AlmaLinux | 8, 9, 10 | `almalinux` container |
| Ubuntu | 22.04, 24.04, 26.04 | container |
| Debian | 12, 13 | container |
| Fedora | 44 | container |
| openSUSE Leap | 16.0 | container |
| Alpine Linux | 3.24 | container |
| FreeBSD | 14.5, 15.1 | VM via `cross-platform-actions/action` |
| NetBSD | 10.1, 11.0 | VM, best effort (Tier 3) |
| OpenBSD | 7.9 | VM, best effort (Tier 3) |

macOS is built and tested by the `macos` job in `.github/workflows/ci.yml`.

Linux is Tier 1 and FreeBSD and macOS are Tier 2: their lanes must pass.
NetBSD and OpenBSD are Tier 3. Their lanes run but do not block merges, and
the workflow names each test that fails there and why.

The dependencies are a C compiler, Autoconf, Automake, Libtool, Net-SNMP,
MariaDB Connector/C, OpenSSL, and cmocka for the unit tests. MySQL's
`libmysqlclient` still builds with `--with-mysql-client=mysql` but is
deprecated and will be removed in the next release
([ADR 0003](adr/0003-mariadb-connector.md)). Spine works with MariaDB and
MySQL servers through either library.
`scripts/test-distros.sh` holds the package list for each platform.

## Known problems

On NetBSD and OpenBSD, `nft_pclose()` spends about a second, not 20 ms,
reaping a script that outlives its pipe, because each of its 100 short
sleeps is rounded up to a 10 ms clock tick. On OpenBSD, a missing PHP
binary is reported by the child's exit status rather than by
`posix_spawn()`, so `php_init()` leaves the server slot busy instead of
failing.

## Running the Linux lanes locally

With Docker installed:

```sh
scripts/test-distros.sh                 # every Linux lane
scripts/test-distros.sh debian:13       # one image
```

The checkout is mounted read-only and copied inside the container, and the
tests run as an unprivileged user. On an arm64 host Docker runs the arm64
images, while CI runs x86-64.
