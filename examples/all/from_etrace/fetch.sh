#!/bin/bash -e
# SPDX-FileCopyrightText: 2026 Accemic Technologies GmbH
# SPDX-License-Identifier: ISC
# -*- tab-width:4; indent-tabs-mode:t -*-
# vim:  tabstop=4:shiftwidth=4:noexpandtab
#
# Copyright (c) 2026 Accemic Technologies GmbH.
#
# Permission to use, copy, modify, and distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
#
#############################################################################
# @brief	Fetch the third-party benchmark inputs for examples/all.
# @author	Albert Schulz <aschulz@accemic.com>
#
# The compression benchmark in examples/all/ runs on RISC-V ELF binaries
# (coremark, dhrystone, embench, riscv-tests, ...) and their spike PC
# traces. These are NOT redistributed in this repo for licensing reasons
# (see README.md). This script reconstructs them locally, on demand, from
# their upstream source: the riscv-trace-spec referenceFlow.
#
# It populates exactly the paths examples/all/makefile expects, so the
# makefile runs unchanged afterwards:
#
#     from_etrace/test_files/*.riscv      (36 ELF binaries, copied as-is)
#     from_etrace/spike/spike.tar.gz      (all *.spike_pc_trace_filtered)
#
# The fetched/generated files are git-ignored and never committed.
#
# Requirements: git, cmake (>=3.10), dtc (device-tree-compiler), python3,
# tar/bzip2. The referenceFlow ships its own pre-built spike + objdump, so
# a full RISC-V toolchain is NOT needed to generate the traces.
#############################################################################

# Upstream source and pinned revision (update deliberately).
RTS_REPO="${RTS_REPO:-https://github.com/riscv-non-isa/riscv-trace-spec.git}"
RTS_COMMIT="${RTS_COMMIT:-40ddc8d1670fd96233bbafe5d8dd77a56ab6e622}"

# This directory (from_etrace/), regardless of caller's CWD.
HERE="$(cd "$(dirname "$0")" && pwd)"
TEST_FILES_DIR="$HERE/test_files"
SPIKE_DIR="$HERE/spike"
SPIKE_TAR="$SPIKE_DIR/spike.tar.gz"

# Cache for the upstream checkout (override with RTS_CACHE).
RTS_CACHE="${RTS_CACHE:-$HERE/.referenceFlow-cache}"
REF="$RTS_CACHE/referenceFlow"

say()  { echo "[fetch] $*"; }
die()  { echo "[fetch] ERROR: $*" >&2; exit 1; }

need() { command -v "$1" >/dev/null 2>&1 || die "missing required tool: $1"; }

#############################################################################
# 0. Idempotency: skip everything if both outputs already exist.
#############################################################################
have_riscv=0
[ -e "$TEST_FILES_DIR" ] && [ "$(ls -1 "$TEST_FILES_DIR"/*.riscv 2>/dev/null | wc -l)" -ge 36 ] && have_riscv=1
have_traces=0
[ -s "$SPIKE_TAR" ] && have_traces=1

if [ "$have_riscv" = 1 ] && [ "$have_traces" = 1 ] && [ "${FORCE:-0}" != 1 ]; then
	say "test_files/*.riscv and spike/spike.tar.gz already present — nothing to do."
	say "(set FORCE=1 to regenerate.)"
	exit 0
fi

need git

#############################################################################
# 1. Clone (or update) the upstream referenceFlow at the pinned commit.
#############################################################################
if [ ! -d "$REF" ]; then
	say "Cloning $RTS_REPO @ $RTS_COMMIT ..."
	mkdir -p "$RTS_CACHE"
	git clone --filter=blob:none --sparse "$RTS_REPO" "$RTS_CACHE/repo" >/dev/null 2>&1 \
		|| die "git clone failed"
	git -C "$RTS_CACHE/repo" sparse-checkout set referenceFlow >/dev/null
	git -C "$RTS_CACHE/repo" checkout -q "$RTS_COMMIT" \
		|| die "git checkout $RTS_COMMIT failed"
	ln -sfn "$RTS_CACHE/repo/referenceFlow" "$REF"
fi
say "Using referenceFlow at $REF"

#############################################################################
# 2. Copy the .riscv ELF binaries as-is.
#############################################################################
if [ "$have_riscv" != 1 ] || [ "${FORCE:-0}" = 1 ]; then
	src_tf="$REF/tests/test_files"
	[ -d "$src_tf" ] || die "expected $src_tf (upstream layout changed?)"
	mkdir -p "$TEST_FILES_DIR"
	say "Copying $(ls -1 "$src_tf"/*.riscv | wc -l) .riscv binaries ..."
	cp -f "$src_tf"/*.riscv "$TEST_FILES_DIR"/
fi

#############################################################################
# 3. Generate the spike PC traces and pack them into spike.tar.gz.
#
# The referenceFlow regression runs spike on every test binary and writes
# <test>.spike_pc_trace_filtered into <regression_dir>/spike/. We then tar
# those filtered traces — exactly the set examples/all/makefile extracts.
#############################################################################
if [ "$have_traces" != 1 ] || [ "${FORCE:-0}" = 1 ]; then
	need cmake; need dtc; need python3; need tar

	if [ ! -x "$REF/build_te/bin/te_codec" ] && [ ! -d "$REF/build_te" ]; then
		say "Building referenceFlow tools (cmake) ..."
		( cd "$REF" && bash scripts/build.sh ) >/dev/null 2>&1 \
			|| die "referenceFlow build failed — run 'bash $REF/scripts/build.sh' to see why"
	fi

	say "Running spike regression to generate PC traces (this can take a few minutes) ..."
	rm -rf "$REF/fetch_reg"
	( cd "$REF" && bash scripts/run_regression.sh --fixed fetch_reg -t itype4_basic ) \
		>/dev/null 2>&1 || die "regression failed — run it manually under $REF to see why"

	gen_spike="$REF/fetch_reg/spike"
	n=$(ls -1 "$gen_spike"/*.spike_pc_trace_filtered 2>/dev/null | wc -l)
	[ "$n" -ge 1 ] || die "no *.spike_pc_trace_filtered produced under $gen_spike"

	mkdir -p "$SPIKE_DIR"
	say "Packing $n filtered traces into spike.tar.gz ..."
	# Store members without a ./ prefix so examples/all/makefile can extract them
	# by bare name (tar -xjf ... coremark.spike_pc_trace_filtered).
	( cd "$gen_spike" && tar -cjf "$SPIKE_TAR" *.spike_pc_trace_filtered )
fi

say "Done."
say "  test_files: $(ls -1 "$TEST_FILES_DIR"/*.riscv 2>/dev/null | wc -l) .riscv binaries"
say "  traces:     $SPIKE_TAR"
say "You can now run the benchmark:  (cd .. && make c)"
