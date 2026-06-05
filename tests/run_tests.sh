#!/bin/bash -e
# -*- tab-width:4; indent-tabs-mode:t -*-
# vim:  tabstop=4:shiftwidth=4:noexpandtab
#############################################################################
# @brief	NexRv regression tests.
# @author	Albert Schulz <aschulz@accemic.com>
#############################################################################

cd "$(dirname "$0")"

NexRv=${NEXRV:-../NexRv.exe}

TmpDir=$(mktemp -d /tmp/nexrv-tests.XXXXXX)
trap 'rm -rf "$TmpDir"' EXIT

compare_files() {
	local actual="$1"
	local expected="$2"
	local label="$3"

	if ! diff "$actual" "$expected"; then
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
# execution trace (roundtrip.pconly) let this run with only NexRv.exe; see
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
$NexRv -conv -objd "$RtDir/roundtrip-OBJD.txt" -pcinfo "$RtPcInfo" >/dev/null
compare_files "$RtPcInfo" "$RtDir/roundtrip.pcinfo" "Round-trip pcinfo (-conv -objd)"

# execution trace -> encoder input sequence.
$NexRv -conv -pcinfo "$RtPcInfo" -pconly "$RtDir/roundtrip.pconly" -pcseq "$RtPcSeq" >/dev/null

for opt in "-nobhm" "-cs 0 -rpt 0" "-cs 8 -rpt 2"; do
	RtNex="$TmpDir/roundtrip.nex"
	RtPcOut="$TmpDir/roundtrip.pcout"
	$NexRv -enco "$RtPcSeq" -nex "$RtNex" $opt >/dev/null
	$NexRv -deco "$RtNex" -pcinfo "$RtPcInfo" -pcout "$RtPcOut" -none >/dev/null
	$NexRv -diff -pconly "$RtDir/roundtrip.pconly" -pcout "$RtPcOut" >/dev/null
	echo "Round-trip ($opt) OK."
done

#############################################################################
# 2. C-Trace capture regression: 'combined' testbench
#
# combined.nex is the raw Nexus trace captured from the C-Trace 'combined'
# testbench (tests/combined/01_all in the C-Trace repo); combined.pcinfo is
# the matching program info. It mixes instruction trace (SYNC / branch /
# call / return) with data-acquisition events (MEMREAD / MEMWRITE). The
# golden CTXP text was produced by NexRv and cross-checked set-equal against
# the testbench's own combined_tb.expected.ctxp.
#############################################################################

CombDir=ctxp/combined
CombOut="$TmpDir/combined.ctxp.txt"

CTXP_TEXT_TRACEFILE="$CombOut" \
	$NexRv -deco "$CombDir/combined.nex" \
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
# against the C-Trace testbench (tip.txt) and ctxp_lint.py.
#############################################################################

CtxpDir=ctxp/csr_cap
CtxpOut="$TmpDir/csr_cap.ctxp.txt"

CTXP_TEXT_TRACEFILE="$CtxpOut" \
	$NexRv -deco   "$CtxpDir/csr_cap.nex" \
	       -pcinfo "$CtxpDir/csr_cap.pcinfo" \
	       -pcout  /dev/null \
	       -none >/dev/null

compare_files "$CtxpOut" "$CtxpDir/csr_cap.expected.ctxp.txt" "DAQ/ACT-CAP CTXP export"
