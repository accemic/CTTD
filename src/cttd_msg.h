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
// File cttd_msg.h  - Nexus RISC-V Trace message definitions (dump & decode)

#ifndef NEXRVMSG_H
#define NEXRVMSG_H

#include "cttd.h"

// Macros to define Nexus Messages (NEXM_...)
//  NOTE: These macros refer to 'NEXUS_TCODE_...' and 'NEXUS_FLDSIZE_...'
//  It is also possible to NOT do it, but it provides less flexibility.
//
//                          name   def (marker | value)
//#define NEXM_BEG(n, t)      {#n,    0x100 | (t)                 }
#define NEXM_BEG(n, t)      {#n,    0x100 | (NEXUS_TCODE_##n)   }
// #define   NEXM_FLD(n, s)    {#n,    0x200 | (s)                 }
#define   NEXM_FLD(n, s)    {#n,    0x200 | (NEXUS_FLDSIZE_##n) }
#define   NEXM_VAR(n)       {#n,    0x400                       }
#define   NEXM_ADR(n)       {#n,    0xC00                       }
#define   NEXM_END()          {NULL,  1                           }

// Definition of Nexus Messages (subset applicable to RISC-V PC trace)
static struct NEXM_MSGDEF_STRU {
  const char *name; // Name of message/field
  int def;          // Definition of field (see NEXM_... above)
} nexusMsgDef[] = {

  NEXM_BEG(DeviceID, 1),
    NEXM_VAR(DEVID),
    NEXM_VAR(TSTAMP),
  NEXM_END(),

  NEXM_BEG(Ownership, 2),
    NEXM_VAR(PROCESS),
    NEXM_VAR(TSTAMP),
  NEXM_END(),

  NEXM_BEG(DirectBranch, 3),
    NEXM_VAR(ICNT),
    NEXM_VAR(TSTAMP),
  NEXM_END(),

  NEXM_BEG(IndirectBranch, 4),
    NEXM_FLD(BTYPE, 2),
    NEXM_VAR(ICNT),
    NEXM_ADR(UADDR),
    NEXM_VAR(TSTAMP),
  NEXM_END(),

  NEXM_BEG(DataWrite, 5),
    NEXM_FLD(DSZ, 4),
    NEXM_FLD(ELSZ, 3),
    NEXM_ADR(DADDR),
    NEXM_VAR(DATA),
    NEXM_VAR(TSTAMP),
  NEXM_END(),

  NEXM_BEG(DataRead, 6),
    NEXM_FLD(DSZ, 4),
    NEXM_FLD(ELSZ, 3),
    NEXM_ADR(DADDR),
    NEXM_VAR(DATA),
    NEXM_VAR(TSTAMP),
  NEXM_END(),

  NEXM_BEG(DataAcquisition, 7),
    NEXM_FLD(IDTAG, 12),
    NEXM_VAR(DQDATA),
    NEXM_VAR(TSTAMP),
  NEXM_END(),

  NEXM_BEG(Error, 8),
    NEXM_FLD(ETYPE, 4),
    NEXM_VAR(ECODE),    // variable-length per RISC-V N-Trace
    NEXM_VAR(TSTAMP),
  NEXM_END(),

  NEXM_BEG(ProgTraceSync, 9),
    NEXM_FLD(SYNC, 4),
    NEXM_VAR(ICNT),
    NEXM_ADR(FADDR),
    NEXM_VAR(TSTAMP),
  NEXM_END(),

  NEXM_BEG(DirectBranchSync, 11),
    NEXM_FLD(SYNC, 4),
    NEXM_VAR(ICNT),
    NEXM_ADR(FADDR),
    NEXM_VAR(TSTAMP),
  NEXM_END(),

  NEXM_BEG(IndirectBranchSync, 12),
    NEXM_FLD(SYNC, 4),
    NEXM_FLD(BTYPE, 2),
    NEXM_VAR(ICNT),
    NEXM_ADR(FADDR),
    NEXM_VAR(TSTAMP),
  NEXM_END(),

  // TCODE 13/14 (P3): synchronizing 5/6 forms. Field order mirrors the CTTE
  // wire layout (get_msg_format(): TCODE, [SRC], DSZ, ELSZ, full address,
  // data, TSTAMP). The address field is deliberately named DADDR -- not ADDR
  // as the RTL format table's debug label -- so NexusFieldGet() and the
  // decode_and_check.sh --data machinery treat 5/6 and 13/14 uniformly (the
  // names here are decoder-local; they have no wire effect). Unlike the 5/6
  // DADDR (XOR delta when compression is on), this DADDR always carries the
  // FULL address.
  NEXM_BEG(DataWriteSync, 13),
    NEXM_FLD(DSZ, 4),
    NEXM_FLD(ELSZ, 3),
    NEXM_ADR(DADDR),
    NEXM_VAR(DATA),
    NEXM_VAR(TSTAMP),
  NEXM_END(),

  NEXM_BEG(DataReadSync, 14),
    NEXM_FLD(DSZ, 4),
    NEXM_FLD(ELSZ, 3),
    NEXM_ADR(DADDR),
    NEXM_VAR(DATA),
    NEXM_VAR(TSTAMP),
  NEXM_END(),

  // TCODE 15 (P4): WPHIT = bitmap of the watchpoints that fired. Variable
  // length like every other CTTE payload field (leading zeros stripped).
  NEXM_BEG(Watchpoint, 15),
    NEXM_VAR(WPHIT),
    NEXM_VAR(TSTAMP),
  NEXM_END(),

  NEXM_BEG(ResourceFull, 27),
    NEXM_FLD(RCODE, 4),
    NEXM_VAR(RDATA),
    NEXM_VAR(HREPEAT),     // Optional: only present when RCODE=2 (HIST_OVERFLOW_REPEATED)
    NEXM_VAR(TSTAMP),
  NEXM_END(),

  NEXM_BEG(IndirectBranchHist, 28),
    NEXM_FLD(BTYPE, 2),
    NEXM_VAR(ICNT),
    NEXM_ADR(UADDR),
    NEXM_VAR(HIST),
    NEXM_VAR(TSTAMP),
  NEXM_END(),

  NEXM_BEG(IndirectBranchHistSync, 29),
    NEXM_FLD(SYNC, 4),
    NEXM_FLD(BTYPE, 2),
    // NEXM_FLD(CANCEL, 1),
    NEXM_VAR(ICNT),
    NEXM_ADR(FADDR),
    NEXM_VAR(HIST),
    NEXM_VAR(TSTAMP),
  NEXM_END(),

  NEXM_BEG(RepeatBranch, 30),
    NEXM_VAR(BCNT),
    NEXM_VAR(TSTAMP),
  NEXM_END(),

  NEXM_BEG(RepeatInstruction, 31),   // Accemic/ISTO 4.3.14: spin-loop compression
    NEXM_VAR(RCNT),
    NEXM_VAR(ICNT),
    NEXM_VAR(HIST),
    NEXM_VAR(TSTAMP),
  NEXM_END(),

  NEXM_BEG(RepeatInstructionSync, 32), // Accemic/ISTO 4.3.15 (synchronizing)
    NEXM_FLD(SYNC, 4),
    NEXM_VAR(RCNT),
    NEXM_VAR(ICNT),
    NEXM_ADR(FADDR),
    NEXM_VAR(HIST),
    NEXM_VAR(TSTAMP),
  NEXM_END(),

  NEXM_BEG(VendorJTC, 57),        // Accemic: IBH with jump-target-cache index
    NEXM_FLD(BTYPE, 2),
    NEXM_VAR(ICNT),
    NEXM_VAR(JIDX),
    NEXM_VAR(HIST),
    NEXM_VAR(TSTAMP),
  NEXM_END(),

  NEXM_BEG(VendorBP, 56),         // Accemic: BCNT predicted branches + 1 mispredict
    NEXM_VAR(BCNT),
    NEXM_VAR(TSTAMP),
  NEXM_END(),

  NEXM_BEG(VendorConfig, 58),     // Accemic: in-band encoder configuration (v1)
    NEXM_FLD(CFGVER, 4),
    NEXM_VAR(CAPS),               // compiled-in feature bitmap
    NEXM_VAR(ENAB),               // runtime enables (same bit positions)
    NEXM_VAR(P0),                 // SrcID[15:4] | SrcBits[3:0]
    NEXM_VAR(P1),                 // InhibitSrc[11] | SyncMax[10:7] | SyncMode[6:3] | InstMode[2:0]
    NEXM_VAR(P2),                 // TsWidth[11:6] | TsPrescale[5:4] | TsType[3:1] | TsEnable[0]
    NEXM_VAR(P3),                 // RetStackDepth[12:8] | BpTableLog2[7:4] | JtcIndexBits[3:0]
    NEXM_VAR(TSTAMP),
  NEXM_END(),

  NEXM_BEG(ProgTraceCorrelation, 33),
    NEXM_FLD(EVCODE, 4),
    NEXM_FLD(CDF, 2),
    NEXM_VAR(ICNT),
    NEXM_VAR(HIST),   // Only if CDF=1!
    NEXM_VAR(TSTAMP),
  NEXM_END(),

  { NULL, 0 } // End-marker ('def == 0' is not otherwise used - see NEXM_...)
};

#endif  // NEXRVMSG_H

//****************************************************************************
// End of cttd_msg.h file
