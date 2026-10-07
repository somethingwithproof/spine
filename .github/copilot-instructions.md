<!--
SPDX-FileCopyrightText: 2026 The Cacti Group
SPDX-License-Identifier: GPL-2.0-or-later
-->

# spine implementation and review instructions

Read [AGENTS.md](../AGENTS.md) for the complete repository-specific guidance.

## Review priorities

Review memory/buffer safety, thread ownership, timeout handling, SQL/SNMP compatibility and credential redaction.

Require focused regression evidence for changed behavior and preserve existing
correctness/security checks. Autotools build in an isolated checkout; runtime polling requires explicit disposable fixtures.

Flag unsupported maturity claims, hidden failures, credentials in code/logs, and
generated output presented as first-party implementation. Review permissions,
immutable Action pins and fork-secret isolation when workflows change. A skipped
check or absent check result is not proof that validation ran.

Keep changes within the requested scope and use `mise` for language runtimes.
