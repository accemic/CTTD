all: NexRv.exe

SRCDIR=src

# Toolchain / flags (override for cross builds, e.g. CC=aarch64-linux-gnu-gcc)
CC      ?= gcc
CFLAGS  ?= -O3
LDFLAGS ?=

# Handle EXTRA=name option (to compile name_ext.c file)
ifdef EXTRA
FEXTRA=$(SRCDIR)/$(EXTRA)_ext.c
WITH_EXT=-DWITH_EXT=1
else
FEXTRA=
WITH_EXT=
endif

SRCS=$(SRCDIR)/NexRv.c $(SRCDIR)/NexRvDeco.c $(SRCDIR)/NexRvEnco.c $(SRCDIR)/NexRvDump.c \
     $(SRCDIR)/NexRvInfo.c $(SRCDIR)/NexRvConv.c $(SRCDIR)/NexRvExport.c $(SRCDIR)/NexRvCTXP.c
HDRS=$(wildcard $(SRCDIR)/*.h)

NexRv.exe : $(SRCS) $(HDRS) $(FEXTRA)
	$(CC) $(CFLAGS) $(WITH_EXT) -I$(SRCDIR) $(SRCS) $(FEXTRA) $(LDFLAGS) -o $@

clean:
	rm -f NexRv.exe

# Diff this fork against upstream tg-nexus-trace refcode/c/ via git difftool.
# See scripts/diff-upstream.sh for details and env-var overrides.
diff-upstream:
	@bash scripts/diff-upstream.sh

.PHONY: all clean diff-upstream
