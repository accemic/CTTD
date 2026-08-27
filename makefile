# SPDX-FileCopyrightText: 2026 Accemic Technologies GmbH
# SPDX-License-Identifier: ISC
all: cttd.exe

SRCDIR=src

# Toolchain / flags (override for cross builds, e.g. CC=aarch64-linux-gnu-gcc)
CC      ?= gcc
CFLAGS  ?= -O3
LDFLAGS ?=

# Version string printed by the banner. Derived from the nearest tag so that a
# binary always says which source it came from: an exact tag build says
# `v2.2.0-cttd2`, a build N commits past it says `v2.2.0-cttd2-N-g<sha>`, and
# a build with local changes appends `-dirty`. A source archive without git
# falls back to the default compiled into src/cttd.c. Override with
# `make CTTD_VERSION=...` if a build system knows better.
CTTD_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null)
ifneq ($(strip $(CTTD_VERSION)),)
VERSION_DEF = -DCTTD_VERSION=\"$(CTTD_VERSION)\"
else
VERSION_DEF =
endif

# Handle EXTRA=name option (to compile name_ext.c file)
ifdef EXTRA
FEXTRA=$(SRCDIR)/$(EXTRA)_ext.c
WITH_EXT=-DWITH_EXT=1
else
FEXTRA=
WITH_EXT=
endif

SRCS=$(SRCDIR)/cttd.c $(SRCDIR)/cttd_deco.c $(SRCDIR)/cttd_enco.c $(SRCDIR)/cttd_dump.c \
     $(SRCDIR)/cttd_info.c $(SRCDIR)/cttd_conv.c $(SRCDIR)/cttd_export.c $(SRCDIR)/cttd_ctxp.c \
     $(SRCDIR)/cttd_decoe.c $(SRCDIR)/cttd_daq.c
HDRS=$(wildcard $(SRCDIR)/*.h)

cttd.exe : $(SRCS) $(HDRS) $(FEXTRA)
	$(CC) $(CFLAGS) $(WITH_EXT) $(VERSION_DEF) -I$(SRCDIR) $(SRCS) $(FEXTRA) $(LDFLAGS) -o $@

# Legacy name: every existing caller (ct_env.sh, dashboard, CI fallbacks,
# board deploys) still looks for NexRv.exe. Kept as a copy so the rename
# is a brand change, not a breaking change.
legacy: cttd.exe
	cp -f cttd.exe NexRv.exe

clean:
	rm -f cttd.exe NexRv.exe

# Diff this fork against upstream riscv-nexus-trace refcode/c/ via git difftool.
# See scripts/diff-upstream.sh for details and env-var overrides.
diff-upstream:
	@bash scripts/diff-upstream.sh

# English-only guard over the tracked text files (see scripts/check_language.py).
check:
	@python3 scripts/check_language.py

.PHONY: all clean legacy diff-upstream check
