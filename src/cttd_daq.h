// SPDX-FileCopyrightText: 2026 Accemic Technologies GmbH
// SPDX-License-Identifier: ISC
// vim: set ts=4 et:
/*
* Copyright (c) 2026 Accemic Technologies GmbH.
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

// Shared DAQ (ACT-CAP DataAcquisition) -> CTXP mapping, used by BOTH the
// N-Trace decoder (cttd_deco.c, Nexus DataAcquisition message) and the E-Trace
// decoder (cttd_decoe.c, vendor msg_type 1). ONE mapping, no drift (§4).

#ifndef ROOT_NEXRVDAQ_H
#define ROOT_NEXRVDAQ_H

#include <stdint.h>
#include "cttd_export.h"

// ACT-CAP command opcodes carried in the DataAcquisition IDTAG field
// (rdl/ct_cs_cpuif.rdl trActCapStCmd_e).
enum {
  ACT_CAP_PC_CURR = 1, ACT_CAP_PC_CURR_LAST = 2, ACT_CAP_DIRECT_DATA = 3,
  ACT_CAP_DATA = 4, ACT_CAP_DADDR = 5, ACT_CAP_DATA_DADDR = 6,
  ACT_CAP_IFETCH_TH = 8, ACT_CAP_DATA_RD_TH = 9, ACT_CAP_DATA_WR = 10,
  ACT_CAP_DATA_RD = 11, ACT_CAP_CF_SYNC = 12, ACT_CAP_TE = 13
};

// tip dtype values (rtl/pkg/tip_pkg.sv tip_dtype_e). Only LOAD is a read; all
// stores / CSR writes / atomics collapse to a write (spec: read/write only).
#define DAQ_DTYPE_LOAD 0

// Map a captured access (dtype, nbytes) to a sized MEMREAD/MEMWRITE event type.
// nbytes is the access size in BYTES (1/2/4/8).
CttdExportEventType DaqMemEvt(unsigned dtype, unsigned nbytes);

// Emit the CTXP event(s) for one DataAcquisition record. cmd = IDTAG (ACT-CAP
// command); el0/el1/el2 = the up-to-three captured 64-bit DQDATA elements
// (element 0 in the LSBs); ts = reconstructed cycle; src = CTXP source id.
void cttd_daq_emit(uint8_t src, unsigned cmd,
                    uint64_t el0, uint64_t el1, uint64_t el2, uint64_t ts);

#endif // ROOT_NEXRVDAQ_H
