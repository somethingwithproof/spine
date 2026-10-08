# Code-scanning review on develop

Reviewed against bf65f988f88845c25a0a3c53f47cbfdf21c8a34a on 2026-10-08.
The latest CodeQL analysis completed without errors or findings. The open
GitHub alerts came from Scorecard and SonarCloud.

## Changes requiring a new scan after merge

- 3181: create new poller logs with mode 0640 instead of 0666. The caller's
  umask can restrict this further; existing log ownership/modes remain intact.
  `test_new_log_permissions` checks creation with umask zero.
- 3217: validate the public PHP reader's process table and index before any
  access, matching command/init/close validation. Invalid slots return an owned
  `U` response; `test_readpipe_rejects_invalid_slots` covers the boundaries and
  a missing process table.
- 3240: CodeQL defaults to contents: read, with upload permissions granted
  only to its analysis job.
- 3241–3248: pin official Debian multi-platform image manifests by digest and
  install CI Python tools from complete hash-checked requirements. Docker
  Dependabot includes the nested SNMPv3 coverage directory.
- 3249: run CodeQL on every push to develop and every pull request, including
  documentation-only changes. Historical coverage does not change retroactively.

## Findings with an existing safety boundary

These are review conclusions, not scanner exclusions. No rule or scan is disabled.

| Alerts | Evidence on the reviewed revision |
| --- | --- |
| 3235–3237 | `decimal_syntax` casts each character to unsigned char before `isdigit`; every ctype table index is in the representable unsigned-char range. |
| 3218 | `read_script_result` reads at most RESULTS_BUFFER - 1 bytes and verifies a positive return strictly below RESULTS_BUFFER before writing the terminator. |
| 3216 | `php_write_no_sigpipe` writes payload bytes to an already-owned pipe descriptor. Payload bytes do not select a pathname or descriptor. |
| 3220 | `log_to_file` obtains its descriptor from the configured log path; its message argument is only the write payload. The separate creation-mode issue is fixed above. |
| 3238, 3226 | `spine_log` sanitizes the fully formatted message before stdout, syslog or file output. `test_spine_regressions` covers ASCII controls, Unicode line separators and truncated UTF-8. |
| 3232 | `spine_snprintf` performs bounded formatting and rejects truncation; it is not a logging sink. Logging callers use the same sanitization boundary. |
| 3231 | The formatted string is the script server's intentional CRLF command protocol. It is not emitted directly as a log record. |
| 3229 | The hostname format builds a Net-SNMP session peer address; subsequent Spine logs go through the sanitizer. |
| 3227–3228 | Error lists format integer data-source IDs, not arbitrary text. Their final log records also go through the sanitizer. |

## Repository governance

3239 reports that develop has no required pull-request review. Existing status
checks and administrator enforcement must be preserved when adding a review
requirement. Requiring one independent approval is a concrete follow-up policy
choice for the repository owner; it will prevent self-approved merges.
3251 reports the historical approval ratio. Future independent review improves
that ratio; approvals must not be fabricated for past changesets.
3252 requires an actual OpenSSF Best Practices assessment. A README badge cannot
truthfully be added before the project has completed that assessment.

PR validation remains offline: no live Cacti database, device polling, privilege
changes, installation, or production deployment.
