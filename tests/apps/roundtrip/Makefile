# SPDX-FileCopyrightText: 2026 Accemic Technologies GmbH
# SPDX-License-Identifier: ISC
# Regenerate the committed round-trip inputs from roundtrip.S.
#
# Only needed when the assembly source changes; the test itself runs from the
# committed roundtrip-OBJD.txt / roundtrip.pconly / roundtrip.pcinfo with just
# cttd.exe. Requires a RISC-V toolchain (riscv32-unknown-elf-gcc/objdump).
#
# NOTE: roundtrip.pconly (the dynamic execution trace) is hand-written to match
# the program's control flow and is NOT regenerated here.

TOOLPREFIX ?= riscv32-unknown-elf-
CC      := $(TOOLPREFIX)gcc
OBJDUMP := $(TOOLPREFIX)objdump
Cttd    := ../../../cttd.exe

CFLAGS  := -nostdlib -nostartfiles -march=rv32im -mabi=ilp32 \
           -Wl,-Ttext=0x1000 -Wl,-e,_start

all: roundtrip-OBJD.txt roundtrip.pcinfo

roundtrip.elf: roundtrip.S
	$(CC) $(CFLAGS) $< -o $@

roundtrip-OBJD.txt: roundtrip.elf
	$(OBJDUMP) -d $< > $@

roundtrip.pcinfo: roundtrip-OBJD.txt
	$(Cttd) -conv -objd $< -pcinfo $@

clean:
	rm -f roundtrip.elf

.PHONY: all clean
