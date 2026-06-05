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

#include "NexRvExport.h"

#include <stddef.h>

#define NEXRV_EXPORT_MAX_CALLBACKS 8

typedef struct ExportSlot {
  NexRvExportCallback cb;
  void *user_data;
} ExportSlot;

static ExportSlot g_slots[NEXRV_EXPORT_MAX_CALLBACKS];
static int g_slot_count = 0;

int nexrv_export_register(NexRvExportCallback cb, void *user_data)
{
  if (cb == NULL) return 0;
  if (g_slot_count >= NEXRV_EXPORT_MAX_CALLBACKS) return 0;

  g_slots[g_slot_count].cb = cb;
  g_slots[g_slot_count].user_data = user_data;
  g_slot_count++;
  return 1;
}

void nexrv_export_emit(const NexRvExportEvent *event)
{
  if (event == NULL) return;
  for (int i = 0; i < g_slot_count; i++)
  {
	if (g_slots[i].cb != NULL)
	  g_slots[i].cb(event, g_slots[i].user_data);
  }
}
