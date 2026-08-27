<!--
SPDX-FileCopyrightText: 2026 Accemic Technologies GmbH
SPDX-License-Identifier: ISC
-->
# CTTD — Changelog

Consumers pin CTTD by tag **and** sha256 (CEDARtools.TraceEncoder:
`scripts/cttd.pin`). This file is the behavioural record per pin: what changed
in the decoder and which verdicts on the consumer side may move because of it.
Binaries are published as GitHub release assets per tag (`cttd-linux-x86_64`,
`cttd-linux-arm64`, `cttd-windows-x64.exe` + `SHA256SUMS`) -- see the README,
"Releases".

## v2.3.0-cttd3 — 2026-08-27

First release published from GitHub (`accemic/CTTD`, the repository formerly
named `NexRv-for-TraceEncoder`) and the first one built by the release
workflow; the earlier CTTD versions below were built and delivered by hand.

- **Decoder hang fixed** (`src/cttd_deco.c`, `EmitICNT`): a branch-history
  field with the MSB 'stop-bit' set made the sliding history mask overflow to
  zero and loop forever. Upstream NexRv v1.0.0 (2025/01/02) carries the guard;
  the fork line (from `v2.0.0` on) had missed it. Visible effect:
  `examples/t1/make` hung on every `-cs`/`-rpt` configuration (only `-nobhm`
  decoded); the regression suite's tiny round-trip program never produced a
  full-width history and could not see it. Consumer effect: streams carrying
  full-width branch histories decode instead of hanging; all other decodes
  are unchanged (golden CTXP exports and round-trips byte-identical).
- `tests/run_tests.sh` gains a round-trip over the `examples/t1` trace for
  `-cs 0 -rpt 0` and `-cs 8 -rpt 2` (the configurations that hung); the CI
  workflow now runs the suite on the x86_64 build before staging artifacts.
- Publication hygiene: `examples/t1/output/` is tracked again (the quick start
  could not write into it from a fresh clone), `LICENSE-Apache-2.0.txt` added
  for the SiFive `entry.S` notice, upstream repository name updated
  (`riscv-nexus-trace`), README build section.

## v2.2.0-cttd2 — 2026-08-17

- **Consolidated the full fork line onto CTTD** (`4e22916`): the E-Trace front
  end (`-decoe`, `src/cttd_decoe.c`), the DAQ layer (`src/cttd_daq.*`) and the
  fuller versions of every shared source file that the internal fork line
  had carried but the first CTTD rename (`v2.1.0-cttd1`) had missed. Consumer effect: the CTTE E-Trace CTXP
  gate (`cli_etrace_ctxp_test.sh`) runs on the pinned build (6/6 legs) instead
  of skipping; N-Trace verdicts unchanged.
- Usage lists `-decoe`; `-conv -objd ... -sijump` documented (2026-08-18,
  this branch head).

## v2.1.0-cttd1 — 2026-08-17

- **Rename NexRv → CEDARtools.TraceDecoder (CTTD)** (`5a69864`): executable
  `cttd.exe` (`make legacy` still emits `NexRv.exe`), sources `src/cttd*.c`,
  identifiers, banner and diagnostics (`Cttd ERROR:`); behaviour-neutral, the
  regression suite and golden CTXP exports pass byte-identically. CLI options,
  environment variables and on-disk formats unchanged.
- CI builds three platforms (`dd37264`): linux-x86_64, linux-arm64 (KV260
  boards), windows-x64 (mingw cross).
- Decoder fixes carried from the internal fork line (`23414b6` …
  `9bb8487`): RepeatBranch head guard for mid-stream ring captures; mirror
  clear on TRACE_ENABLE sync alongside FIFO_OVERRUN; correlation = decoder
  unlock (pause-edge contract); inline SYNC-ENABLE / CORR marks and
  resourceFull ICNT print in the walk trace for per-boundary attribution.

## v2.0.0 — the last state under the old name

- Byte-identical to tag `v2.0.0`, the last state published under this
  repository's previous name, `NexRv-for-TraceEncoder`. Everything after it
  is CTTD.
