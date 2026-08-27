#!/bin/bash -e
# SPDX-FileCopyrightText: 2026 Accemic Technologies GmbH
# SPDX-License-Identifier: ISC
# -*- tab-width:4; indent-tabs-mode:t -*-
# vim:  tabstop=4:shiftwidth=4:noexpandtab
#############################################################################
# @brief	CTTD (CEDARtools.TraceDecoder) regression tests.
# @author	Albert Schulz <aschulz@accemic.com>
#############################################################################

cd "$(dirname "$0")"

# CTTD is the tool; NEXRV stays accepted so existing callers keep working.
Cttd=${CTTD:-${NEXRV:-../cttd.exe}}
if ! [ -x "$Cttd" ] && ! command -v "$Cttd" >/dev/null 2>&1; then
	echo "No decoder at '$Cttd'. Build it first: 'make' in the repository root" \
	     "(or point CTTD=... at a built cttd.exe)." >&2
	exit 2
fi

TmpDir=$(mktemp -d "${TMPDIR:-/tmp}/cttd-tests.XXXXXX")
trap 'rm -rf "$TmpDir"' EXIT

compare_files() {
	local actual="$1"
	local expected="$2"
	local label="$3"

	# --strip-trailing-cr: mingw builds write pcinfo/pcout with CRLF while the
	# committed goldens are LF — byte-compare would fail on line endings alone.
	if ! diff --strip-trailing-cr "$actual" "$expected"; then
		echo "Test failed. $label differs from reference."
		exit 1
	else
		echo "$label OK."
	fi
}

#############################################################################
# 1. Assembler encode -> decode round-trip
#
# apps/roundtrip/roundtrip.S is a tiny hand-written RISC-V program that
# exercises every control-flow edge the decoder must reconstruct: a taken
# and not-taken conditional branch (loop back-edge), a direct call and its
# return, and a direct jump. The committed objdump (roundtrip-OBJD.txt) and
# execution trace (roundtrip.pconly) let this run with only cttd.exe; see
# apps/roundtrip/Makefile to regenerate them from the .S with a RISC-V
# toolchain.
#
# Flow: objdump -> pcinfo, pconly -> pcseq, then for several encoder
# compression levels: encode -> decode -> assert the decoded PC stream is
# byte-identical to the original execution trace.
#############################################################################

RtDir=apps/roundtrip
RtPcInfo="$TmpDir/roundtrip.pcinfo"
RtPcSeq="$TmpDir/roundtrip.pcseq"

# objdump -> pcinfo (exercises -conv -objd); must match the committed golden.
$Cttd -conv -objd "$RtDir/roundtrip-OBJD.txt" -pcinfo "$RtPcInfo" >/dev/null
compare_files "$RtPcInfo" "$RtDir/roundtrip.pcinfo" "Round-trip pcinfo (-conv -objd)"

# execution trace -> encoder input sequence.
$Cttd -conv -pcinfo "$RtPcInfo" -pconly "$RtDir/roundtrip.pconly" -pcseq "$RtPcSeq" >/dev/null

for opt in "-nobhm" "-cs 0 -rpt 0" "-cs 8 -rpt 2"; do
	RtNex="$TmpDir/roundtrip.nex"
	RtPcOut="$TmpDir/roundtrip.pcout"
	$Cttd -enco "$RtPcSeq" -nex "$RtNex" $opt >/dev/null
	$Cttd -deco "$RtNex" -pcinfo "$RtPcInfo" -pcout "$RtPcOut" -none >/dev/null
	$Cttd -diff -pconly "$RtDir/roundtrip.pconly" -pcout "$RtPcOut" >/dev/null
	echo "Round-trip ($opt) OK."
done

#############################################################################
# 2. CTTE capture regression: 'combined' testbench
#
# combined.nex is the raw Nexus trace captured from the CTTE 'combined'
# testbench (tests/combined/01_all in the TraceEncoder repository); combined.pcinfo is
# the matching program info. It mixes instruction trace (SYNC / branch /
# call / return) with data-acquisition events (MEMREAD / MEMWRITE). The
# golden CTXP text was produced by NexRv (the pre-rename tool) and cross-checked set-equal against
# the testbench's own combined_tb.expected.ctxp.
#############################################################################

CombDir=ctxp/combined
CombOut="$TmpDir/combined.ctxp.txt"

CTXP_TEXT_TRACEFILE="$CombOut" \
	$Cttd -deco "$CombDir/combined.nex" \
	       -pcinfo "$CombDir/combined.pcinfo" \
	       -pcout  /dev/null \
	       -none >/dev/null

compare_files "$CombOut" "$CombDir/combined.expected.ctxp.txt" "Combined capture CTXP export"

#############################################################################
# 3. DAQ / ACT-CAP -> CTXP export regression (csr_cap fixture)
#
# Exercises all DataAcquisition command kinds (PC_CURR, PC_CURR_LAST,
# DIRECT_DATA, DATA, DADDR, DATA_DADDR, the four counters) plus the derived
# SYNC / MEMREAD / MEMWRITE / DAQ_* CTXP events. Golden output was verified
# against the CTTE testbench (tip.txt) and ctxp_lint.py.
#############################################################################

CtxpDir=ctxp/csr_cap
CtxpOut="$TmpDir/csr_cap.ctxp.txt"

CTXP_TEXT_TRACEFILE="$CtxpOut" \
	$Cttd -deco   "$CtxpDir/csr_cap.nex" \
	       -pcinfo "$CtxpDir/csr_cap.pcinfo" \
	       -pcout  /dev/null \
	       -none >/dev/null

compare_files "$CtxpOut" "$CtxpDir/csr_cap.expected.ctxp.txt" "DAQ/ACT-CAP CTXP export"

#############################################################################
# 4. examples/t1 round-trip (164959-instruction RLE program, upstream example)
#
# The roundtrip.S program above is too short to produce full-width branch
# histories. The t1 trace does, and its history messages carry the MSB
# 'stop-bit' -- the case that made EmitICNT() spin forever until the upstream
# stop-bit guard was carried over (see src/cttd_deco.c). Both configurations
# with branch-history messages hung before that fix.
#############################################################################

T1Dir=../examples/t1
T1PcInfo="$TmpDir/t1.pcinfo"
T1PcSeq="$TmpDir/t1.pcseq"

$Cttd -conv -objd "$T1Dir/test-OBJD.txt" -pcinfo "$T1PcInfo" >/dev/null
$Cttd -conv -pcinfo "$T1PcInfo" -pconly "$T1Dir/test-PCONLY.txt" -pcseq "$T1PcSeq" >/dev/null

for opt in "-cs 0 -rpt 0" "-cs 8 -rpt 2"; do
	T1Nex="$TmpDir/t1.nex"
	T1PcOut="$TmpDir/t1.pcout"
	$Cttd -enco "$T1PcSeq" -nex "$T1Nex" $opt >/dev/null
	$Cttd -deco "$T1Nex" -pcinfo "$T1PcInfo" -pcout "$T1PcOut" -none >/dev/null
	$Cttd -diff -pconly "$T1Dir/test-PCONLY.txt" -pcout "$T1PcOut" >/dev/null
	echo "t1 round-trip ($opt) OK."
done
