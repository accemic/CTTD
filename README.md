<!--
SPDX-FileCopyrightText: 2026 Accemic Technologies GmbH
SPDX-License-Identifier: ISC
-->

# CEDARtools.TraceDecoder (CTTD)

**The reference decoder for [CEDARtools.TraceEncoder (CTTE)](https://github.com/accemic/TraceEncoder):
RISC-V N-Trace and E-Trace back to a PC stream, plus CTXP export. Plain C99, ISC.**

> Formerly **NexRv for C-Trace**. Derived from the RISC-V Nexus Trace TG
> reference code: <https://github.com/riscv-non-isa/riscv-nexus-trace/tree/main/refcode/c>

CTTD is Accemic's trace decoder. It started as a fork of NexRv, the Nexus
Trace TG reference decoder, and has since grown well past what that name
describes -- which is why the name changed:

- **Nexus is no longer the whole story.** CTTD already decodes the CTXP export
  format, data-trace messages, vendor TCODEs, branch prediction and multi-source
  (SRC) streams; the Nexus message set is one input format among several.
- **More source formats are planned:** Infineon AURIX and ARM ETM trace are the
  next targets. A name built on "Nexus RISC-V" would misdescribe the tool.
- CTTD is the decoding counterpart to
  [CEDARtools.TraceEncoder (CTTE)](https://github.com/accemic/TraceEncoder),
  whose every decode verdict is taken with a build of this tool pinned by tag
  **and** sha256 (`scripts/cttd.pin` there).

## Quick start

```sh
make                        # -> ./cttd.exe (gcc, plain C99, no dependencies)
./cttd.exe                  # prints the banner (version) and the usage
bash tests/run_tests.sh     # regression suite: round-trips + golden CTXP exports
```

A worked example with commentary is [`examples/t1/README.md`](examples/t1/README.md)
(`cd examples/t1 && make` encodes and decodes a 164 959-instruction program five
ways and diffs every result against the recorded PC sequence).

## What the rename did and did not touch

| Renamed | Left alone (deliberately) |
|---|---|
| Executable: `NexRv.exe` -> **`cttd.exe`** (`make legacy` still produces `NexRv.exe` as a copy) | Command-line options (`-deco`, `-enco`, `-dump`, `-conv`, `-diff`, `-bp`, `-cs`, ...) -- unchanged, every script keeps working |
| Source files: `src/NexRv*.c/h` -> `src/cttd*.c/h` | Environment variables `NEXRV_SRC_BITS`, `NEXRV_TRACE_FROM`, `NEXRV_CSDBG` -- renaming them would silently change behaviour for existing callers |
| Internal identifiers `NexRv*` / `nexrv_*` / `NEXRV_*` -> `Cttd*` / `cttd_*` / `CTTD_*` (the include guards kept their old spelling) | On-disk formats: `.nex`, `.pcinfo`, `.pcout`, `.ctxp` -- byte-compatible |
| Banner, usage text, diagnostics (`NexRv ERROR:` -> `Cttd ERROR:`; the E-Trace front end says `CttdDecoE ERROR:`) | The ISC license text and both copyright lines (see below) |
| Test runner, release workflow, roundtrip makefile | `CTXP_TRACEFILE` / `CTXP_TEXT_TRACEFILE` (format-named, not tool-named) |

The rename is intended to be **behaviour-neutral**: the regression suite
(`tests/run_tests.sh`) passes unchanged, including the byte-compared
round-trips and the golden CTXP exports.

## License and provenance -- please keep this intact

CTTD is **ISC-licensed** ([LICENSE](LICENSE)) and carries **two** copyright
holders. Both must appear in every copy; the ISC text says so explicitly
("provided that the above copyright notice and this permission notice appear
in all copies"):

```
Copyright (c) 2020 IAR Systems AB.
Copyright (c) 2026 Accemic Technologies GmbH.
```

- **IAR Systems AB** holds the copyright on the NexRv base -- the RISC-V Nexus
  Trace TG reference code this tool derives from. The rename to CTTD changes
  the product name, **not** the authorship of that base. Files that still
  contain base code carry the IAR line plus the note *"Modified from the
  RISC-V Nexus Trace TG reference code."*
- **Accemic Technologies GmbH** holds the copyright on the fork's additions
  (CTXP export, data trace, vendor TCODEs, multi-target/SRC decoding,
  branch-prediction walk, call-stack validation and the CTTD rename itself).
- Files written entirely at Accemic (`cttd_ctxp.*`, `cttd_export.*`,
  `cttd_target.h`, `cttd_daq.*`, `cttd_decoe.c`) carry only the Accemic
  line -- correct, because no base code is in them. Files with substantial
  Accemic changes on top of base code (`cttd.c`, `cttd_deco.c`, `cttd_conv.c`,
  `cttd_info.c`, ...) carry both lines.
- Every file declares its license with an SPDX tag or, where a file cannot
  carry one, in [`REUSE.toml`](REUSE.toml); the license texts are under
  [`LICENSES/`](LICENSES/) and the repository is [REUSE](https://reuse.software)-compliant.
- The reference PDF [NexusTraceTG-RefCode.pdf](./NexusTraceTG-RefCode.pdf)
  remains **CC-BY-4.0** as upstream ([LICENSES/CC-BY-4.0.txt](LICENSES/CC-BY-4.0.txt)).
- The `examples/t1` startup files inherited from the upstream examples keep
  their own third-party notices: `entry.S` is SiFive, **Apache-2.0**
  ([LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt)); `crt0.S` is SiFive under
  the FreeBSD license (SPDX `BSD-2-Clause-Views`, see its file header).
  `test.elf` is the upstream prebuilt example binary; it additionally contains
  code from [xrle](https://github.com/algorithm314/xrle) (BSD-2-Clause) and
  newlib -- its provenance and an open licensing question are recorded in
  [`LICENSES/LicenseRef-Upstream-t1-Example.txt`](LICENSES/LicenseRef-Upstream-t1-Example.txt).
- Third-party benchmark inputs under [examples/all](examples/all/) are not
  committed for licensing reasons; reconstruct them with
  [examples/all/from_etrace/fetch.sh](examples/all/from_etrace/fetch.sh).
- "RISC-V" is used **descriptively** here (Accemic is not a RISC-V
  International member); "CEDARtools" is an Accemic brand.

## What CTTD adds over the NexRv base

- [CTXP](https://github.com/accemic/CTXP-format) trace export (CEDARtools.TraceEncoder eXPort format)
- Data message decoding
- Optional SRC field decoding/encoding and **multi-target decode contexts** (`-src N -target N -pcinfo .. -pcout ..`, repeatable -- one context per source in a funnel-merged stream)
- **E-Trace front end** (`-decoe`): RISC-V Efficient-Trace `te_inst` packets to the same PC stream / CTXP export
- **DAQ / data-acquisition messages** (`cttd_daq.*`), incl. the Accemic vendor packets in the E-Trace container
- Timestamps (absolute and relative mode)
- Vendor TCODEs (jump-target cache, branch prediction, configuration)
- Call-stack decoding treated as a validation check (`-csstrict`: a return whose explicit target disagrees with the mirror is fatal)
- `-conv -objd ... -sijump`: sequentially-implicit-jump folding for cores that advertise
  [CTTE](https://github.com/accemic/TraceEncoder)'s `CT_SIJUMP` (auipc/lui+jalr pairs)
- Helper script to compare against upstream: [scripts/diff-upstream.sh](scripts/diff-upstream.sh)

## Building

Plain C99 against libc only, no external dependencies. Prerequisites: `gcc`
(or a compatible C compiler) and `make`.

    make                                # -> ./cttd.exe (native build)
    make CC=aarch64-linux-gnu-gcc       # Linux arm64 cross build
    make CC=x86_64-w64-mingw32-gcc      # Windows x64 cross build (mingw-w64)
    make legacy                         # additionally copies cttd.exe -> NexRv.exe
    make check                          # English-only guard over the tracked files

The executable is named `cttd.exe` on every platform, Linux included (a
convention inherited from upstream NexRv). The banner prints the version the
makefile derives from `git describe --tags`: an exact tag build says
`v2.2.0-cttd2`, a build past a tag says `v2.2.0-cttd2-N-g<sha>`, a build with
local changes appends `-dirty`, so a binary always tells you which source it
came from. `.github/workflows/release.yml` builds the same three targets in CI.
The test suite (`tests/run_tests.sh`, below) is a bash script; on Windows it
needs a POSIX shell (e.g. Git Bash or WSL).

## Releases and how consumers get the binaries

Tags `vX.Y.Z-cttdN` mark releases (see [CHANGELOG.md](CHANGELOG.md)). Pushing
a tag makes `.github/workflows/release.yml` build the three binaries, run the
regression suite on the x86_64 one, and publish them as the assets of a
[GitHub Release](https://github.com/accemic/CTTD/releases):
`cttd-linux-x86_64`, `cttd-linux-arm64` (what the KV260 boards run),
`cttd-windows-x64.exe`, plus a `SHA256SUMS` in `sha256sum -c` format.

Consumers pin a release by tag **and** sha256 --
[CEDARtools.TraceEncoder](https://github.com/accemic/TraceEncoder) does that
in `scripts/cttd.pin` and fetches with `scripts/fetch_cttd.py`. That is why a
release with assets is **sealed**: the workflow never overwrites a published
asset, and a fix is always a new tag. Every push to `main` additionally
refreshes a rolling `main-rolling` pre-release for people who want to try the
head; nothing pins it.

The earlier CTTD versions `v2.1.0-cttd1` and `v2.2.0-cttd2` predate this
repository: their binaries were built by hand (Linux host, gcc 10.5, mingw-w64
for Windows) and delivered directly to their consumers, whose pins carry those
digests. They are not republished here -- a rebuilt binary would not match the
pinned bytes, and that mismatch is exactly what a pin exists to catch. GitHub
releases start with `v2.3.0-cttd3`.

### Tests

Run the regression suite with [tests/run_tests.sh](tests/run_tests.sh) (needs only the built `cttd.exe`, so run `make` first):

- **Assembler round-trip** — a tiny hand-written RISC-V program ([tests/apps/roundtrip/roundtrip.S](tests/apps/roundtrip/roundtrip.S)) is converted to a trace by the encoder, decoded again, and the decoded PC stream is checked byte-for-byte against the original execution. Repeated for several encoder compression levels.
- **CTTE capture regression** — real Nexus traces captured from the open [CEDARtools.TraceEncoder](https://github.com/accemic/TraceEncoder) testbenches (`combined` and `csr_cap`) are decoded to CTXP and compared against golden references cross-checked against the testbenches' own expected output.
- **`examples/t1` round-trip** — the 164959-instruction upstream example program is encoded with branch-history messages (`-cs 0 -rpt 0`, `-cs 8 -rpt 2`) and decoded back; its full-width histories cover the stop-bit path that the short program above cannot reach.

## Related repositories

| Repository | Relation |
|---|---|
| [accemic/TraceEncoder](https://github.com/accemic/TraceEncoder) — CEDARtools.TraceEncoder | the encoder IP this decoder is the reference for; pins CTTD by sha256 |
| [accemic/CTXP-format](https://github.com/accemic/CTXP-format) | the specification of the CTXP export this tool writes |
| `NexRv-for-TraceEncoder` | this repository's previous name: the `v2.0.0` line at the start of its git history, published before the rename to CTTD |
| [riscv-non-isa/riscv-nexus-trace](https://github.com/riscv-non-isa/riscv-nexus-trace) | upstream: the N-Trace specification and the NexRv reference code |

Security issues: see [SECURITY.md](SECURITY.md). Maintainers are routed by
[`.github/CODEOWNERS`](.github/CODEOWNERS); general contact <info@accemic.com>.

### *Acknowledgment - TRISTAN EU project*

This fork was developed as part of the TRISTAN project, a European Union research initiative involving 46 partners to advance the RISC-V ecosystem. The TRISTAN project, nr. 101095947 is supported by Chips Joint Undertaking (CHIPS-JU) and its members Austria, Belgium, Bulgaria, Croatia, Cyprus, Czechia, Germany, Denmark, Estonia, Greece, Spain, Finland, France, Hungary, Ireland, Iceland, Italy, Lithuania, Luxembourg, Latvia, Malta, Netherlands, Norway, Poland, Portugal, Romania, Sweden, Slovenia, Slovakia, Turkey. See https://tristan-project.eu/ for more information.

## Updated 2025/01/02 - upstream

* Support for -cs and -rpt added (to control callstack mode and repeat messages).
* Support for 64-bit.
* Scripts to run all examples added.
* Make other enhancements.

Top level overview is provided here: [NexusTraceTG-RefCode.pdf](./NexusTraceTG-RefCode.pdf).

* Source (DOC) for above PDF is lost and this PDF  does not exactly correspon to current version.
* Newest usage can be seen by running `cttd.exe` with no arguments.
* Also [./examples/t1/makefile](./examples/t1/makefile) and [./examples/all/makefile](./examples/all/makefile) show different usage examples.

Example with additional instructions and usage details is [here](./examples/t1/README.md).
