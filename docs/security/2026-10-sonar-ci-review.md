# SonarCloud develop CI review

The analysis of develop revision `9ba9a7cf209deb1df421bd4a9520ae2f2d1669f9`
failed reliability, security and new-code coverage conditions. The quality gate
requires 80% new-code coverage; its measured result was 69.6%. Thresholds,
analysis rules and production source inclusion remain unchanged.

## Implementation and measurement fixes

Two `c:S5000` reports identified comparisons of whole `struct in6_addr` objects
in the direct IPv6 receiver and shared reply dispatcher. Both now compare only
the standard `s6_addr` byte arrays. Peer identity ignores implementation padding
and still uses all 16 address octets. The shared-dispatch test rejects a mismatch
in every individual octet before accepting the matching address.

The coverage runner previously built and ran only its production-linked
regression/fault programs. Its Sonar and regression jobs also lacked cmocka,
so `make check` omitted the optional unit suites. Those suites exercise shipped
IPv6 and syscall failure implementations but contributed no coverage profiles.
Both jobs now install cmocka. The runner executes each configured unit suite,
records its actual object producers, verifies gcov notes and profiles, and saves
the binary/profile/source hashes and test logs alongside the existing linked
regression evidence. The strict production-source verifier is unchanged.

Additional contracts assert configured/missing option values, lower/upper clamps,
CLI override precedence and fixture-table restoration. Startup output tests
cover primary/remote pollers, verbosity and console visibility. Availability
tests cover failure/recovery thresholds, initial states and SNMP requirements,
checking response-time statistics and error messages as well as final state.

## Security reports requiring individual review

The following reports concern existing safety boundaries in the reviewed
revision. Their conclusions require source and test evidence; a green gate alone
does not establish correctness. Formatting command, SQL or peer-address bytes
must not apply log sanitization and change those protocols.

| Issue key suffix / location | Source and regression evidence |
| --- | --- |
| `cQLa`, `app/buffer.c:31` | `spine_snprintf` calls bounded `vsnprintf` to format memory; it emits no log record. Actual logging separately sanitizes its final message. |
| `cQME`, `log/log.c:172` | `write` sends message bytes to the descriptor returned by opening the configured log path. The message does not select a pathname or descriptor. Real-file tests verify append, framing and creation permissions. |
| `cQMF`, `log/log.c:224` | The formatter is followed by `spine_sanitize_log_message` before every output destination. Real-file tests assert injected CR/LF form exactly one record; sanitizer tests cover ASCII controls, DEL, NEL, Unicode separators and truncated UTF-8. |
| `cQLi`, `poller/items.c:265` | `snprintf` copies an OID into its bounded request buffer; this is not a log sink. SNMP batch tests exercise request and result handoff. |
| `cQLf`, `cQLg`, `cQLh`, `poller/result.c:57,74,75` | Each `isdigit` argument is cast to `unsigned char`, satisfying the ctype table-index contract. Numeric classification and output-parser fuzz tests cover valid and invalid responses. |
| `cQLn`, `script/external.c` | `read` requests at most `RESULTS_BUFFER - 1` bytes. The terminator write is guarded by `bytes > 0 && bytes < RESULTS_BUFFER`. Real child tests exercise empty, exact-full and oversized output and timeout cleanup. |
| `cQL_`, `script/protocol.c:117` | `write` sends command payload to the owned PHP child pipe, not to a payload-selected path. Real-child protocol tests exercise framing, short writes, closed peers and shutdown. |
| `cQL3`, `script/read.c:198` | The process table and slot index are validated before access. The descriptor is checked against zero and `FD_SETSIZE`; the response read length reserves a terminator byte. Slot, descriptor and stream-boundary tests exercise those guards. |
| `cQMK`, `snmp/session.c:452` | The bounded formatter constructs Net-SNMP's peer address in memory; it is not a logging operation. Session tests exercise transport-qualified hosts and failures; actual logs use the sanitizer. |
| `cQLj`, `cQLk`, `poller/poller.c:487,492` | The formatters render integer data-source IDs into error-list buffers. Actual output passes through the sanitized logger; numeric error-list boundary contracts exercise batching. |
| `cQMA`, `script/protocol.c:156` | The formatter adds intentional CRLF protocol framing to a bounded PHP request. It is not a log record. Script stream and real-child tests verify this protocol. |

These are sink/bounds-model false positives, not accepted vulnerabilities.
All 14 security reports in the table were individually reviewed and recorded
as false positives against the unchanged code on develop, with source hashes
and successful real-file, child-process, bounds and production-linked tests.
Sonar's security rating then returned to A. Review states and explanations
belong on the individual Sonar issues; no
blanket rule exclusion, `NOSONAR`, coverage exclusion or gate relaxation is
introduced. Newly reported findings must be reviewed independently.
