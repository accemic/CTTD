// SPDX-FileCopyrightText: 2020 IAR Systems AB
// SPDX-FileCopyrightText: 2026 Accemic Technologies GmbH
// SPDX-License-Identifier: ISC
/*
* Copyright (c) 2020 IAR Systems AB.
* Copyright (c) 2026 Accemic Technologies GmbH.
*
* Modified from the RISC-V Nexus Trace TG reference code.
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
* WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
* MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
* ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
* WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
* ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
* OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

//****************************************************************************
// File cttd.h  - Nexus RISC-V Trace header.

// Header with common Nexus Trace definitions
// Used by Cttd*.c (dump, encode, decode, convert)

#ifndef CTTD_H
#define CTTD_H

#include <stdint.h> // For uint32_t and uint64_t

//****************************************************************************
// Nexus specific values (based on Nexus Standard PDF)

#define NEXUS_FLDSIZE_TCODE                       6 // This is standard

// Nexus TCODE values applicable to RISC-V
// Nexus standard TCODE 1 (Device ID, IEEE-ISTO 5001 4.3.2): a single static
// identifier message the encoder emits once at trace start (CTTE:
// trTeControl.SendDeviceId = DID_ONCE, payload = the encoder instance's
// CT_DEVICE_ID elaboration parameter, layout per ISTO Table B-5). N-Trace 1.0
// does not adopt it (Table 9), so CTTE keeps it runtime-off by default.
// CTTE emits the ID field VENDOR_VARIABLE (leading zeros stripped) rather
// than the "Fixed 32" of Table 4-7 -- a documented deviation; the variable
// form is what this table describes. PC-neutral: no ICNT, no address.
#define NEXUS_TCODE_DeviceID                      1
#define NEXUS_TCODE_Ownership                     2
#define NEXUS_TCODE_DirectBranch                  3
#define NEXUS_TCODE_IndirectBranch                4
#define NEXUS_TCODE_DataWrite                     5
#define NEXUS_TCODE_DataRead                      6
#define NEXUS_TCODE_DataAcquisition               7
#define NEXUS_TCODE_Error                         8
#define NEXUS_TCODE_ProgTraceSync                 9
#define NEXUS_TCODE_DirectBranchSync              11
#define NEXUS_TCODE_IndirectBranchSync            12
// Nexus standard TCODEs 13/14 (Data Trace Write/Read with Sync): the
// synchronizing forms of 5/6. N-Trace 1.0 does not adopt data trace at all
// (Table 9), so like 5/6 these follow the Nexus/ISTO layout as emitted by
// CTTE: with trTeDataAddrCompress != FULL (P3, CT_EN_DF_ADDR_COMPRESS) the
// encoder upgrades the first data message after a re-anchor event (sync /
// DataTracing rising edge / ERROR / reset) to 13/14 carrying the FULL data
// address; subsequent 5/6 carry the XOR against the previous data message's
// address. 13/14 are synchronizing: absolute TSTAMP (N-Trace 8.5 rule).
#define NEXUS_TCODE_DataWriteSync                 13
#define NEXUS_TCODE_DataReadSync                  14
// Nexus standard TCODE 15 (Watchpoint, IEEE-ISTO 5001 4.3.23): WPHIT is the
// bitmap of the watchpoints that fired. CTTE drives it from the ACT-ST
// command ACT_CAP_ST_WATCHPOINT masked by trWpMask.WEM, so the bit<->slot
// assignment is a software convention of the traced system. N-Trace 1.0 does
// not adopt it either; PC-neutral like the Device ID message.
#define NEXUS_TCODE_Watchpoint                    15
#define NEXUS_TCODE_ResourceFull                  27
#define NEXUS_TCODE_IndirectBranchHist            28
#define NEXUS_TCODE_IndirectBranchHistSync        29
#define NEXUS_TCODE_RepeatBranch                  30
#define NEXUS_TCODE_RepeatInstruction             31  // Accemic/ISTO 4.3.14 (reserved in N-Trace 1.0)
#define NEXUS_TCODE_RepeatInstructionSync         32  // Accemic/ISTO 4.3.15
#define NEXUS_TCODE_ProgTraceCorrelation          33
// Accemic CTTE vendor extension (Nexus vendor TCODE range 56..62):
// IndirectBranchHist whose target comes from a 64-entry jump-target cache
// (JIDX field) instead of a differential UADDR. Encoder and decoder keep
// bit-identical cache models: entry = full byte address, index = 6-bit XOR
// fold (a>>2 ^ a>>8 ^ a>>14 ^ a>>20) & 0x3F, updated by every emitted plain
// (BTYPE=0-eligible) IndirectBranchHist, read (not updated) by VendorJTC.
#define NEXUS_TCODE_VendorJTC                     57
// Accemic CTTE vendor extension (TCODE 56): branch prediction. Both sides
// run a bit-identical predictor (2^9 direct-mapped 2-bit saturating counters,
// index = (pc>>2)&0x1FF, init weakly-not-taken, updated with the actual
// outcome of every resolved direct branch). With BP enabled (-bp) direct
// branches carry no HIST bits: the decoder resolves every branch it walks
// from its own predictor. TCODE 56 marks the exception: BCNT correctly
// predicted branches since the last PC-walking message, then exactly ONE
// branch whose outcome is the INVERSE of the prediction. The decoder walks
// those BCNT+1 branches eagerly (like ResourceFull RCODE 1/2, no ICNT field,
// walked halfwords subtracted from the next ICNT-bearing packet).
#define NEXUS_TCODE_VendorBP                      56
// Accemic CTTE vendor extension (TCODE 58): config message. The encoder
// describes its own configuration in-band as the first message of a trace
// session (SPEC_config_message.md v1): CFGVER fixed(4), then variable CAPS
// (compiled-in feature bitmap), ENAB (runtime enables, same positions),
// P0 (SrcID|SrcBits), P1 (InhibitSrc|SyncMax|SyncMode|InstMode),
// P2 (TsWidth|TsPrescale|TsType|TsEnable), P3 (RetStackDepth|BpTableLog2|
// JtcIndexBits), TSTAMP. The decoder auto-configures from it (ENAB.5 ->
// -bp walk, P0/P1 -> -src) unless the corresponding CLI flag was given
// explicitly (CLI wins). PC-neutral for the walk itself.
#define NEXUS_TCODE_VendorConfig                  58

// End of standard values
//****************************************************************************

//****************************************************************************
// RISC-V Nexus Trace related values (most 'recommended' by Nexus)

//  Sizes of fields:
#define NEXUS_FLDSIZE_BTYPE     2 // Branch type
#define NEXUS_FLDSIZE_SYNC      4 // Synchronization code
#define NEXUS_FLDSIZE_ETYPE     4 // Error type
#define NEXUS_FLDSIZE_CFGVER    4 // Accemic config message format version
#define NEXUS_FLDSIZE_ECODE     8 // Error code
#define NEXUS_FLDSIZE_EVCODE    4 // Event code (correlation)
#define NEXUS_FLDSIZE_CDF       2
#define NEXUS_FLDSIZE_RCODE     4 // Resource full code
#define NEXUS_FLDSIZE_DSZ       4 // size of the write/read
#define NEXUS_FLDSIZE_ELSZ      3 // size of the element within the data access
#define NEXUS_FLDSIZE_IDTAG     12 // DAQ message ID Tag width
#define NEXUS_HIST_BITS         31 // Number of valid HIST bits

// Address skipping (on RISC-V LSB of PC is always 0, so it is not encoded)
#define NEXUS_PARAM_AddrSkip    1
#define NEXUS_PARAM_AddrUnit    1

#if 0   // 32-bit only version (initial reference code)
typedef uint32_t Nexus_TypeAddr;
typedef uint32_t Nexus_TypeHist;
typedef uint32_t Nexus_TypeField;   // This must be same (or bigger than Addr/Hist fields)
#else   // 64-bit (for practical use-cases)
typedef uint64_t Nexus_TypeAddr;
typedef uint32_t Nexus_TypeHist;
typedef uint64_t Nexus_TypeField;   // This must be same (or bigger than Addr/Hist fields)
#endif

extern int cttd_conf_src_bits;    // 0 = no SRC field (default), 1-6 = SRC bit width
extern int cttd_conf_src_filter;  // -1 = all sources (default), 0+ = only this source
extern int cttd_conf_src_value;   // SRC value to encode (default 0)

// Address width of the decoded stream (R1.2 / X2c). The decoder STATE is
// always 64-bit (Nexus_TypeAddr); this flag only selects the TEXT width of
// every address the N-Trace front-end prints, mirroring the encoder-side
// reference model cpu_model.sv: 8 hex digits while the core is RV32, 16 once
// it is RV64. The encoder announces it in-band with CAPS bit 23 (ADDR64) of
// the config message (TCODE 58, CFGVER stays 1) -- there is deliberately NO
// CLI switch, so a capture can never be decoded with a width its producer did
// not declare. Without a config message the flag stays 0: a width is a
// printf MINIMUM, so an address beyond 2^32 still prints in full, just wider.
//
// PER TRACE SOURCE, not per process (R1.2b, audit finding B-1): this variable
// is the ACTIVE target's width, swapped by NexusDecoStateSave/Restore against
// CttdDecoState.confAddr64 -- exactly like conf_BranchPredict and conf_DfXor.
// A funnel-merged stream may mix XLEN (two RV32 cores plus one RV64 core is
// the trio build of this repository); with a process-global flag the RV64
// source's config message widened EVERY source's pcout. cttd_target.h states
// the contract: a TCODE-58 field overrides only the emitting source's context.
extern int cttd_conf_addr64;      // active target: 0 = 32-bit text, 1 = 64-bit

// End of RISC-V related values
//****************************************************************************

#endif  // CTTD_H

//****************************************************************************
// End of cttd.h file
