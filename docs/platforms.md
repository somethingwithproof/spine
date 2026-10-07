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
| Alpine Linux | 3.24 | container, failures do not block (see below) |
| FreeBSD | 14.5, 15.1 | VM via `cross-platform-actions/action` |
| NetBSD | 10.1, 11.0 | VM via `cross-platform-actions/action` |
| OpenBSD | 7.9 | VM via `cross-platform-actions/action` |

macOS is built and tested by the `macos` job in `.github/workflows/ci.yml`.

The dependencies are a C compiler, Autoconf, Automake, Libtool, Net-SNMP,
a MariaDB or MySQL client library, OpenSSL, and cmocka for the unit tests.
`scripts/test-distros.sh` holds the package list for each platform.

## Known problems

On Alpine (musl), `make check` fails. `poll_host()` in `poller.c` uses about
145 KiB of stack, and musl gives new threads 128 KiB by default, so a worker
thread overflows its stack. The Alpine lane does not block merges until that
is fixed.

## Running the Linux lanes locally

With Docker installed:

```sh
scripts/test-distros.sh                 # every Linux lane
scripts/test-distros.sh debian:13       # one image
```

The checkout is mounted read-only and copied inside the container, and the
tests run as an unprivileged user. On an arm64 host Docker runs the arm64
images, while CI runs x86-64.
