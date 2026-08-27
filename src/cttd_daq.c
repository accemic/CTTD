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

// Shared DAQ (ACT-CAP) -> CTXP mapping. Extracted verbatim from the N-Trace
// DataAcquisition handler so both protocol front-ends produce identical CTXP.

// stddef.h for NULL: cttd_daq.h does not pull it in itself, and older
// toolchains do not deliver it indirectly either. On Windows / newer GCC this
// never showed; the aarch64 cross build for the board (gcc 9) fails with
// "'NULL' undeclared" without it.
#include <stddef.h>

#include "cttd_daq.h"

CttdExportEventType DaqMemEvt(unsigned dtype, unsigned nbytes)
{
  int isWrite = (dtype != DAQ_DTYPE_LOAD);
  switch (nbytes)
  {
    case 1:  return isWrite ? CTTD_EXPORT_EVT_MEMWRITE_1 : CTTD_EXPORT_EVT_MEMREAD_1;
    case 2:  return isWrite ? CTTD_EXPORT_EVT_MEMWRITE_2 : CTTD_EXPORT_EVT_MEMREAD_2;
    case 4:  return isWrite ? CTTD_EXPORT_EVT_MEMWRITE_4 : CTTD_EXPORT_EVT_MEMREAD_4;
    case 8:  return isWrite ? CTTD_EXPORT_EVT_MEMWRITE_8 : CTTD_EXPORT_EVT_MEMREAD_8;
    default: return isWrite ? CTTD_EXPORT_EVT_MEMWRITE_0 : CTTD_EXPORT_EVT_MEMREAD_0;
  }
}

static void Emit(uint8_t src, CttdExportEventType type,
                 uint64_t v1, uint64_t v2, uint64_t cycle)
{
  CttdExportEvent e;
  e.source_id = src;
  e.type = type;
  e.value1 = v1;
  e.value2 = v2;
  e.cycle_count = cycle;
  e.comment = NULL;
  cttd_export_emit(&e);
}

void cttd_daq_emit(uint8_t src, unsigned cmd,
                    uint64_t el0, uint64_t el1, uint64_t el2, uint64_t ts)
{
  switch (cmd)
  {
    case ACT_CAP_PC_CURR: { // (DAQ_DATA tag ->) SYNC(target=PC)
      uint64_t pc = el0, tag = el1;
      if (tag != 0) Emit(src, CTTD_EXPORT_EVT_DAQ_DATA, 0, tag, ts);
      Emit(src, CTTD_EXPORT_EVT_SYNC, 0, pc, ts);
    } break;

    case ACT_CAP_PC_CURR_LAST: { // (tag ->) DAQ_LAST_PC -> SYNC
      uint64_t pc = el0, lastpc = el1, tag = el2;
      if (tag != 0) Emit(src, CTTD_EXPORT_EVT_DAQ_DATA, 0, tag, ts);
      Emit(src, CTTD_EXPORT_EVT_DAQ_LAST_PC, 0, lastpc, ts);
      Emit(src, CTTD_EXPORT_EVT_SYNC, 0, pc, ts);
    } break;

    case ACT_CAP_DIRECT_DATA: // DAQ_DATA(tag) - always emitted
      Emit(src, CTTD_EXPORT_EVT_DAQ_DATA, 0, el0, ts);
      break;

    case ACT_CAP_DATA: { // (tag ->) MEMx_N(value, addr omitted)
      uint64_t value = el0, dd = el1, tag = el2;
      unsigned dtype = (unsigned)((dd >> 6) & 0xF), dsize = (unsigned)(dd & 0x3F);
      if (tag != 0) Emit(src, CTTD_EXPORT_EVT_DAQ_DATA, 0, tag, ts);
      Emit(src, DaqMemEvt(dtype, 1u << dsize), CTTD_EXPORT_ADDR_OMITTED, value, ts);
    } break;

    case ACT_CAP_DADDR: { // (tag ->) MEMx_0(addr)
      uint64_t addr = el0, dd = el1, tag = el2;
      unsigned dtype = (unsigned)((dd >> 6) & 0xF);
      int isWrite = (dtype != DAQ_DTYPE_LOAD);
      if (tag != 0) Emit(src, CTTD_EXPORT_EVT_DAQ_DATA, 0, tag, ts);
      Emit(src, isWrite ? CTTD_EXPORT_EVT_MEMWRITE_0 : CTTD_EXPORT_EVT_MEMREAD_0,
           addr, 0, ts);
    } break;

    case ACT_CAP_DATA_DADDR: { // (tag ->) MEMx_N(addr, value)
      // el2 = {DirectData[23:0], dtype_dsize[9:0]}
      uint64_t value = el0, addr = el1;
      unsigned dd10 = (unsigned)(el2 & 0x3FF);
      uint64_t tag = (el2 >> 10) & 0xFFFFFF;
      unsigned dtype = (dd10 >> 6) & 0xF, dsize = dd10 & 0x3F;
      if (tag != 0) Emit(src, CTTD_EXPORT_EVT_DAQ_DATA, 0, tag, ts);
      Emit(src, DaqMemEvt(dtype, 1u << dsize), addr, value, ts);
    } break;

    case ACT_CAP_IFETCH_TH: case ACT_CAP_DATA_RD_TH:
    case ACT_CAP_DATA_WR:   case ACT_CAP_DATA_RD: {
      // DAQ_COUNTER(count, value2 = [20:19]kind|[18:16]region|[15:0]tag).
      unsigned kind = (cmd == ACT_CAP_IFETCH_TH)  ? 0 :
                      (cmd == ACT_CAP_DATA_RD_TH) ? 1 :
                      (cmd == ACT_CAP_DATA_WR)    ? 2 : 3;
      uint64_t v2 = ((uint64_t)kind << 19);
      Emit(src, CTTD_EXPORT_EVT_DAQ_COUNTER, el0, v2, ts);
    } break;

    case ACT_CAP_CF_SYNC:
      Emit(src, CTTD_EXPORT_EVT_SYNC, 0, el0, ts);
      break;

    case ACT_CAP_TE:        // tracing-enable control: no trace event
    default:
      break;
  }
}
