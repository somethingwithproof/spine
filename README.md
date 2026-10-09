# Spine: Cacti's multithreaded poller

[![CI](https://github.com/somethingwithproof/spine/actions/workflows/ci.yml/badge.svg?branch=develop)](https://github.com/somethingwithproof/spine/actions/workflows/ci.yml)
[![Production regressions](https://github.com/somethingwithproof/spine/actions/workflows/regressions.yml/badge.svg?branch=develop)](https://github.com/somethingwithproof/spine/actions/workflows/regressions.yml)
[![CodeQL](https://github.com/somethingwithproof/spine/actions/workflows/codeql.yml/badge.svg?branch=develop)](https://github.com/somethingwithproof/spine/actions/workflows/codeql.yml)
[![License](https://img.shields.io/github/license/somethingwithproof/spine)](LICENSE)

Spine is a multithreaded C poller for [Cacti](https://www.cacti.net/). It replaces
`cmd.php` as the polling engine, collects device data through SNMP and scripts,
checks availability, and writes results to the Cacti database. Worker threads
allow multiple devices to be polled concurrently.

This fork modernizes Spine while preserving its Cacti database, command-line
and script-server interfaces. Current `develop` uses C17, pthreads and Autotools,
with implementation modules grouped by responsibility. The fork has not yet
published a release; the source version is `1.3.0`.

## Capabilities

- SNMP polling and ICMP, UDP and TCP availability checks.
- External script execution and reusable PHP script-server processes.
- Database connection pools, batched result writes and remote-poller modes.
- Production-linked regression tests for polling, result formatting, process
  cleanup and failure handling, alongside sanitizer and fuzz checks.

Spine supplies the polling engine. Cacti manages devices, data sources,
scheduling and the rest of the monitoring application.

## Requirements and compatibility

Build dependencies are a C compiler, Make, Autoconf, Automake, Libtool,
Net-SNMP, MariaDB Connector/C and OpenSSL development files. Tests also use
cmocka; GNU help2man is included in the documented development prerequisites.

| Requirement | Current develop |
| --- | --- |
| Language | C17 with GNU extensions (`-std=gnu17`) and POSIX/BSD APIs; documented compiler floor: GCC 8 or Clang 6 |
| Platforms | Linux, macOS and FreeBSD have CI lanes; NetBSD and OpenBSD are tested on a best-effort basis; Windows/Cygwin has no CI lane |
| Architecture | Supported builds are 64-bit; configure checks for a 64-bit `time_t` |
| Database client | MariaDB Connector/C is preferred and connects to both MariaDB and MySQL servers |

See [platform releases and support tiers](docs/platforms.md) and
[the C17 decision](docs/adr/0001-c17-language-standard.md).
MySQL's `libmysqlclient` remains available through `--with-mysql-client=mysql`,
but is deprecated. See [the client-library decision](docs/adr/0003-mariadb-connector.md).
The old MySQL 5.0/5.1 instructions used `--with-reentrant`; that option is no
longer provided by this branch.

## Build from source

On Debian or Ubuntu, install the development prerequisites:

```sh
sudo apt-get update
sudo apt-get install build-essential autoconf automake libtool help2man \
  libmariadb-dev libsnmp-dev libssl-dev libcmocka-dev
```

Clone this fork and build `develop`:

```sh
git clone --branch develop https://github.com/somethingwithproof/spine.git
cd spine
./bootstrap
./configure --prefix=/usr/local/spine --with-mysql-client=mariadb --enable-warnings
make -j2
make check
./spine --version
./spine --help
```

Build and test as an ordinary user. `bootstrap` regenerates Autotools files and
can normalize line endings; build in a disposable copy when you need to preserve
an unchanged checkout. For other platforms, consult the package lists in
[scripts/test-distros.sh](scripts/test-distros.sh).

The prefix defaults to `/usr/local/spine`. Set `--prefix` to change it, and use
`./configure --help` for build options. If selecting `libmysqlclient`, point
`MYSQL_CONFIG` at that library's `mysql_config`; some systems provide a tool
with that name from MariaDB Connector/C instead.

## Install and configure

After building and testing, install into the selected prefix:

```sh
sudo make install
```

With the default prefix, the binary is `/usr/local/spine/bin/spine` and the
configuration is `/usr/local/spine/etc/spine.conf`. Installation seeds a missing
configuration from [spine.conf.dist](etc/spine.conf.dist) and preserves an existing
one. Edit it for your Cacti database, replace the sample credentials, and restrict
access to the account that runs the poller.

| Configuration | Purpose |
| --- | --- |
| `DB_Host`, `DB_Port`, `DB_Database` | Cacti database endpoint and name |
| `DB_User`, `DB_Pass` | Database credentials |
| `DB_UseSSL`, `DB_SSL_Key`, `DB_SSL_Cert`, `DB_SSL_CA` | Database TLS settings |
| `RDB_*` | Remote database endpoint, credentials and TLS settings |
| `SNMP_Clientaddr` | Optional source address for SNMP requests |
| `Cacti_Log` | Optional Cacti log path |

Runtime polling settings come from Cacti's `settings` table. Use `-C` to select
configuration explicitly. Without it, Spine searches the current directory,
`/etc`, `/etc/cacti` and `../etc` in that order; it does not automatically search
all installation prefixes.

For a deliberate diagnostic poll of selected Cacti device IDs:

```sh
/usr/local/spine/bin/spine -C /usr/local/spine/etc/spine.conf \
  --hostlist=1,2 --readonly --stdout --verbosity=3
```

Replace `1,2` with the IDs you intend to poll. `--readonly` suppresses database
output; it still connects to the database, contacts devices and executes the
configured scripts. Without a host list or first/last range, Spine processes
all hosts. See the [manual](docs/man/spine.1) for the command-line interface.

Once configuration and polling work, set Cacti's **Paths** setting to the Spine
binary and select **Spine** as its **Poller Type**. Cacti then invokes Spine in
place of `cmd.php`.

### ICMP privileges

For ICMP access on Linux, prefer `CAP_NET_RAW` on the binary or an appropriately
configured `net.ipv4.ping_group_range` for unprivileged ICMP sockets. Without
ICMP access, Spine falls back to UDP with a warning.

For installations that still require setuid root, the historical setup is:

```sh
sudo chown root:root /usr/local/spine/bin/spine
sudo chmod u+s /usr/local/spine/bin/spine
```

This is an alternative installation choice, not a required build step. When
started setuid, Spine opens its ICMP sockets before reading configuration and
drops root permanently; with `--enable-lcap`, it retains only `CAP_NET_RAW`.
It exits if it cannot confirm that root was dropped. A process launched directly
by root retains root. See [SECURITY.md](SECURITY.md) for the privilege model.

## Operational considerations

**Database connections.** Each Spine process opens a main connection and a
pool with one connection per worker thread. Remote polling can open another
main connection and pool against the remote database. PHP scripts may open
additional connections; include those and Cacti's other workloads in capacity
planning.

The previous README's example remains useful when each PHP server also needs
one connection: four processes, ten threads and five PHP servers per process
imply approximately `4 × (1 + 10 + 5) = 64` connections. This is a planning
estimate, not a fixed connection count guaranteed by Spine.

**Trusted configuration.** Spine executes script commands stored in the Cacti
database as the poller account. Protect database write access and the
credential-bearing configuration file. New poller log files use mode `0640`,
subject to the caller's umask; existing file permissions are preserved.

## Windows / Cygwin notes

These notes preserve the historical workflow. This branch has no Windows CI
lane, so they are a starting point rather than a verified support claim. Native
Windows work is described in the [roadmap](ROADMAP.md).

Use the [Cygwin installer](https://www.cygwin.com/) and its **Install from
Internet** option, then select a mirror. The older default root was `C:\cygwin`;
a 64-bit installation commonly uses `C:\cygwin64`. The previously documented
Spine build packages are:

```text
autoconf automake dos2unix gcc-core gzip help2man inetutils-src
libmariadb-devel libssl-devel libtool m4 make net-snmp-devel openssl-devel wget
```

Open a Cygwin shell, obtain this fork's source and follow the build steps above.
For an archive, extract it under `/usr/src` with
`tar xzvf cacti-spine-<version>.tar.gz`, enter the extracted directory and run
`./bootstrap`. [Cacti's Spine downloads](https://www.cacti.net/spine_download.php)
contain upstream releases, not this fork's changes.

Configure `spine.conf`, test with the explicit configuration and diagnostic
flags above, and set Cacti's executable path. For a 64-bit Cygwin installation
with the default prefix, the path is
`C:\cygwin64\usr\local\spine\bin\spine.exe`.

The Cygwin TCP-ping path currently makes only one connection attempt, even when
retries are configured. If that affects availability checks, select another
reachability method or use a tested Unix platform.

## Development and testing

`make check` runs unit and regression suites. CI also exercises GCC and Clang,
macOS, Linux distributions, native BSD guests, database/SNMP integration,
coverage, static analysis, sanitizers and fuzz targets.

Build and test one Linux distribution in an isolated Docker container with:

```sh
scripts/test-distros.sh debian:13
```

The script copies the checkout into the container and builds as an unprivileged
user. See [CONTRIBUTING.md](CONTRIBUTING.md) for local build and review
requirements, [tests/README.md](tests/README.md) for disposable integration
fixtures, and [docs/ci.md](docs/ci.md) for CI details.

### Source layout

All production C files live under `src/`, grouped by responsibility. Module
headers stay beside their implementations. `include/spine/` is reserved for
deliberately supported external interfaces; this executable currently exposes
no installed SDK. See [source module boundaries](docs/architecture/source-modules.md).

| Location | Contents |
| --- | --- |
| `src/` | Application, configuration, poller, SNMP, ping, database, script, process, platform and logging modules |
| `vendor/uthash/` | Unmodified third-party hash-table header |
| `etc/`, `docs/man/` | Configuration template and command-line manual |
| `tests/contracts/` | Production-linked behavior contracts |
| `tests/unit/`, `tests/regression/` | Unit suites and shell regression checks |
| `tests/support/`, `tests/fixtures/` | Shared test support, PHP protocol fixture and database/agent fixtures |
| `tests/fuzz/` | Fuzz targets and input corpus |
| `scripts/` | Build, distribution, debugging and packaging helpers |
| `docker/`, `packaging/` | Development/coverage images and distribution packaging |
| `.github/` | Workflows, reusable actions, issue/PR templates and review routing |
| `docs/` | Architecture decisions, CI and operational documentation |

Run helper scripts from the checkout root. `bash scripts/package.sh --help`
describes the archive builder; `scripts/debug.sh` expects a configured Makefile
build and launches the debugger.

## Direction and contributions

The [roadmap](ROADMAP.md) covers planned configuration validation, a CMake build,
native Windows support and an event-driven poller. Current `develop` still uses
Autotools and pthreads. [Architecture decisions](docs/adr/README.md) explain the
language, database client and upstream-tracking choices.

Report bugs and propose changes through this repository's
[issues](https://github.com/somethingwithproof/spine/issues) and
[pull requests](https://github.com/somethingwithproof/spine/pulls). Contributions
target `develop`, include relevant tests, and require a DCO sign-off; see
[CONTRIBUTING.md](CONTRIBUTING.md). Report vulnerabilities privately through
[SECURITY.md](SECURITY.md).

## License

See [LICENSE](LICENSE) for the repository's license text and the notices in
individual source files for their licensing terms.

Copyright © 2004–2026 The Cacti Group, Inc.
