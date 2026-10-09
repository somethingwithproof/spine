# Source module boundaries

Spine uses ordinary C translation units grouped by responsibility. Module
headers stay beside their implementations;
`*_internal.h` headers describe only collaboration inside the implementation.
There are no implementation-file includes in production. Include-based legacy
unit tests retain their syscall interception by including the moved sources.

| Directory | Responsibility |
| --- | --- |
| `src/app/` | Application entry, CLI, startup, process-wide runtime ownership and shutdown |
| `src/config/` | Configuration models, defaults, settings cache, database-backed options and file parsing |
| `src/database/` | Connections, pool ownership, remote poller transfers |
| `src/ping/` | Address parsing, UDP/TCP probes, IPv4/IPv6 ICMP, shared raw-socket reply dispatch |
| `src/poller/` | Cycle coordination, workers, hosts, reindexing, item batches, numerical results and system information |
| `src/script/` | External scripts, PHP server lifecycle, protocol framing and response reading |
| `src/process/` | Close-on-exec descriptors, pipes, spawn attributes, bounded child reaping |
| `src/platform/` | OS privileges, thread behavior and synchronization |
| `src/log/` | Logging, formatting and sanitization |
| `src/snmp/` | Sessions, requests, responses and security-protocol selection |

Entry points and subsystem coordinators live alongside their modules in `src/`.
Automake source groups are shared by production and linked tests. The fuzz
build includes the shared runtime sources used by its targets. Autotools remains
the build system.

The split keeps the original function bodies and public signatures. Private
helpers that collaborate across translation units have declarations in internal
headers; helpers used by only one translation unit retain static linkage.
Shared ICMP state remains protected by LOCK_ICMP, including registration,
reply dispatch and ownership handoff. Worker accounting, SQL retry budgets,
script framing, result ownership and shutdown sequencing are unchanged.

At extraction, all 308 original root-source function bodies matched a lexical
comparison with the new module set. Compiler/linker checks and existing runtime
contracts provide the behavior validation; a text comparison alone is not a
runtime acceptance test. Production files range up to approximately 680 lines,
with most below 500. Tests and historical fixtures are separate from that count.

## Repository conventions

`include/spine/` is reserved for deliberately supported external interfaces.
No external SDK is currently installed. The legacy compatibility headers in
`src/internal/` retain shared definitions while focused interfaces replace them.
Private `*_internal.h` headers stay beside the subsystem they serve. The vendored
uthash header lives under `vendor/uthash/` and remains byte-identical.

`etc/spine.conf.dist` supplies the installed configuration template, and
`docs/man/spine.1` supplies the manual. Installation paths and executable names
are unchanged. Contract tests live under `tests/contracts/`; the regression and
fault-test entry points live under `tests/regression/`.

The root keeps Autotools entry points, the default Dockerfile and conventional
project documents. Development/coverage Dockerfiles live under `docker/`, release
helpers under `scripts/`, and platform packaging under `packaging/`. GitHub
workflows, actions, templates and review-routing files live under `.github/`.
Autotools' generated `config/` directory is distinct from the tracked `etc/` tree.
