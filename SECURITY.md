<!--
SPDX-FileCopyrightText: 2026 The Cacti Group
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Security policy

## Supported versions

Security fixes go to the `develop` branch and to the latest release. This
fork has not published a release yet, so today only `develop` is supported.

## Reporting a vulnerability

Report vulnerabilities privately through GitHub Security Advisories on this
repository:
<https://github.com/somethingwithproof/spine/security/advisories/new>.

Do not open a public issue or pull request for a vulnerability. Include the
commit you tested, how Spine was built and installed (setuid, `setcap` or
neither), the steps to reproduce, and the impact you observed.

Reports about upstream Cacti or Spine releases belong with the Cacti
project, through its own security policy.

## Trust model

Spine trusts the Cacti database. It reads its device list, poller items and
script commands from that database and acts on them without further checks.
Spine runs every script and script server command named there as the user
that runs Spine. Anyone who can change those tables, including a Cacti
administrator through the web interface, can therefore run commands as the
Spine user. Reports that need write access to the Cacti database or Cacti
administrator rights are out of scope unless they cross a boundary beyond
that, for example by gaining root.

The Spine configuration file holds the database credentials. Keep it
readable only by the Spine user.

## Privileges

Spine needs raw sockets only to send ICMP pings. If the binary is setuid
root, Spine opens its raw ICMP sockets, or with `--enable-lcap` keeps only
`CAP_NET_RAW`, and then drops root permanently before it reads any option,
file or database row. It exits if it cannot confirm that root is gone. When
root runs Spine directly, for example from root's crontab, Spine keeps root.

Setuid root is not the recommended setup. Prefer one of these:

- give the binary `CAP_NET_RAW` with `setcap cap_net_raw+ep`;
- allow unprivileged ICMP sockets for the poller's group through
  `net.ipv4.ping_group_range`.

Without either, ICMP pings fall back to UDP with a warning.

## Logging

Script and script server commands can carry SNMP communities and SNMPv3
passphrases in their arguments. Spine does not log those arguments at any
log level; log lines name only the first word of the command, the script
itself. Spine does not log database passwords.
