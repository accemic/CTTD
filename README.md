> Fork of https://github.com/riscv-non-isa/riscv-nexus-trace/tree/main/refcode/c

# Nexus Trace TG reference code - for C-Trace
Reference code for RISC-V Nexus Trace TG recommendations.

## Updated 2026/06/01 - C-trace fork

This fork adds decoding capabilities related to the open [C-Trace encoder RTL implementation](https://github.com/accemic/c-trace).

- Add [CTXP](https://github.com/accemic/c-trace-export-format) trace export
- Add data message decoding
- Add optional SRC field decoding/encoding
- Pass timestamps (absolut and relative mode)
- Helper script to compare against upstream at [scripts/diff-upstream.sh]()
- Treat call-stack decoding as validation check
- The third-party benchmark inputs for [examples/all](examples/all/) (RISC-V ELF binaries and Spike PC traces) are not committed, for licensing reasons. Run [examples/all/from_etrace/fetch.sh](examples/all/from_etrace/fetch.sh) to reconstruct them from upstream, then point `RV_OBJDUMP` in the makefile at your own toolchain `objdump`. See [examples/all/from_etrace/README.md](examples/all/from_etrace/README.md) for sources and licenses.
- Code is licensed under [ISC](LICENSE) (per the source headers); the [reference PDF](./NexusTraceTG-RefCode.pdf) remains [CC-BY-4.0](LICENSE-CC-BY-4.0.txt) as upstream.

### Tests

Run the regression suite with [tests/run_tests.sh](tests/run_tests.sh) (needs only the built `NexRv.exe`):

- **Assembler round-trip** — a tiny hand-written RISC-V program ([tests/apps/roundtrip/roundtrip.S](tests/apps/roundtrip/roundtrip.S)) is converted to a trace by the encoder, decoded again, and the decoded PC stream is checked byte-for-byte against the original execution. Repeated for several encoder compression levels.
- **C-Trace capture regression** — real Nexus traces captured from the open [C-Trace](https://github.com/accemic/c-trace) testbenches (`combined` and `csr_cap`) are decoded to CTXP and compared against golden references cross-checked against the testbenches' own expected output.

### *Acknowledgment - TRISTAN EU project*

This fork was developed as part of the TRISTAN project, a European Union research initiative involving 46 partners to advance the RISC-V ecosystem. The TRISTAN project, nr. 101095947 is supported by Chips Joint Undertaking (CHIPS-JU) and its members Austria, Belgium, Bulgaria, Croatia, Cyprus, Czechia, Germany, Denmark, Estonia, Greece, Spain, Finland, France, Hungary, Ireland, Iceland, Italy, Lithuania, Luxembourg, Latvia, Malta, Netherlands, Norway, Poland, Portugal, Romania, Sweden, Slovenia, Slovakia, Turkey. See https://tristan-project.eu/ for more information.

## Updated 2025/01/02 - upstream

* Support for -cs and -rpt added (to control callstack mode and repeat messages).
* Support for 64-bit.
* Scripts to run all examples added.
* Make other enhancements.

Top level overview is provided here: [NexusTraceTG-RefCode.pdf](./NexusTraceTG-RefCode.pdf).

* Source (DOC) for above PDF is lost and this PDF  does not exactly correspon to current version.
* Newest usage can be seen by running `NexRv.exe`.
* Also [./examples/t1/makefile](./examples/t1/makefile) and [./examples/all/makefile](./examples/all/makefile) show different usage examples.

Example with additional instructions and usage details is [here](./examples/t1/README.md).
