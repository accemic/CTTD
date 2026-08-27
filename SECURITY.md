<!--
SPDX-FileCopyrightText: 2026 Accemic Technologies GmbH
SPDX-License-Identifier: ISC
-->

# Security policy

CTTD is a command-line decoder that parses trace bytes, object dumps and
address-info files it is handed. A security issue here is a crafted input
that makes it crash, hang or write outside its output files -- the kind of
thing a fuzzer finds. Reports are welcome.

Please **do not** open a public issue for a vulnerability. Send it to
<info@accemic.com> with the affected version (the banner `cttd.exe` prints),
the input that triggers it, and whether you want to be credited. You will get
an acknowledgement within a few working days; fixes ship as a normal tagged
release with a note in [`CHANGELOG.md`](CHANGELOG.md).

The published binaries are pinned by sha256 by their consumers
(CEDARtools.TraceEncoder, `scripts/cttd.pin`); a release with assets is never
overwritten -- a fix is always a new tag.
