// vim: set ts=4 et:
// -*- indent-tabs-mode: t; tab-width: 4 -*-
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

/*
 * @brief   Generic export dispatch layer for NexRv.
 *
 * This module provides a neutral event interface between the decoder and
 * the individual export formats (CTXP, ...).
 */

#ifndef ROOT_NEXRVEXPORT_H
#define ROOT_NEXRVEXPORT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NexRvExportEventType {
  NEXRV_EXPORT_EVT_SYNC = 0,
  NEXRV_EXPORT_EVT_BRANCH_TAKEN,
  NEXRV_EXPORT_EVT_BRANCH_NOTTAKEN,
  NEXRV_EXPORT_EVT_CALL,
  NEXRV_EXPORT_EVT_RETURN,
  NEXRV_EXPORT_EVT_INTERRUPT,
  NEXRV_EXPORT_EVT_RFI,
  NEXRV_EXPORT_EVT_OVERFLOW,
  NEXRV_EXPORT_EVT_CONTEXT,
  NEXRV_EXPORT_EVT_WALLCLOCK,
  NEXRV_EXPORT_EVT_INFO1,
  NEXRV_EXPORT_EVT_INFO2,
  NEXRV_EXPORT_EVT_INFO3,

  // Data events. value1 = addr (or NEXRV_EXPORT_ADDR_OMITTED when only the
  // value is known), value2 = value. The _0 forms carry an address only.
  NEXRV_EXPORT_EVT_MEMREAD_0,
  NEXRV_EXPORT_EVT_MEMREAD_1,
  NEXRV_EXPORT_EVT_MEMREAD_2,
  NEXRV_EXPORT_EVT_MEMREAD_4,
  NEXRV_EXPORT_EVT_MEMREAD_8,
  NEXRV_EXPORT_EVT_MEMWRITE_0,
  NEXRV_EXPORT_EVT_MEMWRITE_1,
  NEXRV_EXPORT_EVT_MEMWRITE_2,
  NEXRV_EXPORT_EVT_MEMWRITE_4,
  NEXRV_EXPORT_EVT_MEMWRITE_8,

  // Instrumentation / DAQ events.
  NEXRV_EXPORT_EVT_DAQ_DATA,    // value2 = opaque tag (DirectData, 24 bits)
  NEXRV_EXPORT_EVT_DAQ_COUNTER, // value1 = count, value2 = [20:19]kind|[18:16]region|[15:0]tag
  NEXRV_EXPORT_EVT_DAQ_LAST_PC  // value2 = last PC before exception/interrupt
} NexRvExportEventType;

// Sentinel for a memory access whose address is not known (value-only capture).
// Text encoding leaves value1 empty; binary encoding stores this in addr.
#define NEXRV_EXPORT_ADDR_OMITTED (~(uint64_t)0)

typedef struct NexRvExportEvent {
  uint8_t source_id;
  NexRvExportEventType type;
  uint64_t value1;
  uint64_t value2;
  uint64_t cycle_count;
  const char *comment; // Optional; may be NULL
} NexRvExportEvent;

typedef void (*NexRvExportCallback)(const NexRvExportEvent *event, void *user_data);

// Register a callback. Safe to call from constructor.
// Returns 1 on success, 0 on failure.
int nexrv_export_register(NexRvExportCallback cb, void *user_data);

// Emit event to all registered exporters.
void nexrv_export_emit(const NexRvExportEvent *event);

// Convenience helper for control-flow events (origin/target).
static inline void nexrv_export_emit_cf(uint8_t source_id,
										NexRvExportEventType type,
										uint64_t origin,
										uint64_t target,
										uint64_t cycle_count,
										const char *comment)
{
  NexRvExportEvent e;
  e.source_id = source_id;
  e.type = type;
  e.value1 = origin;
  e.value2 = target;
  e.cycle_count = cycle_count;
  e.comment = comment;
  nexrv_export_emit(&e);
}

#ifdef __cplusplus
}
#endif

#endif // ROOT_NEXRVEXPORT_H
