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

#include "NexRvCTXP.h"

#include "NexRvExport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *g_ctxp_bin = NULL;
static FILE *g_ctxp_txt = NULL;

static void ctxp_open_if_env_set(void) __attribute__((constructor));
static void ctxp_close_files(void) __attribute__((destructor));

static void ctxp_on_event(const NexRvExportEvent *event, void *user_data);

static void write_u16_le(FILE *f, unsigned int v)
{
  unsigned char b[2];
  b[0] = (unsigned char)(v & 0xFF);
  b[1] = (unsigned char)((v >> 8) & 0xFF);
  (void)fwrite(b, 1, 2, f);
}

static void write_u64_le(FILE *f, unsigned long long v)
{
  unsigned char b[8];
  for (int i = 0; i < 8; i++)
	b[i] = (unsigned char)((v >> (8 * i)) & 0xFF);
  (void)fwrite(b, 1, 8, f);
}

static const char *ctxp_type_to_string(NexRvExportEventType t)
{
  switch (t)
  {
	case NEXRV_EXPORT_EVT_SYNC:             return "SYNC";
	case NEXRV_EXPORT_EVT_BRANCH_TAKEN:     return "BRANCH_TAKEN";
	case NEXRV_EXPORT_EVT_BRANCH_NOTTAKEN:  return "BRANCH_NOTTAKEN";
	case NEXRV_EXPORT_EVT_CALL:             return "CALL";
	case NEXRV_EXPORT_EVT_RETURN:           return "RETURN";
	case NEXRV_EXPORT_EVT_INTERRUPT:        return "INTERRUPT";
	case NEXRV_EXPORT_EVT_RFI:              return "RFI";
	case NEXRV_EXPORT_EVT_OVERFLOW:         return "OVERFLOW";
	case NEXRV_EXPORT_EVT_CONTEXT:          return "CONTEXT";
	case NEXRV_EXPORT_EVT_WALLCLOCK:        return "WALLCLOCK";
	case NEXRV_EXPORT_EVT_INFO1:            return "INFO1";
	case NEXRV_EXPORT_EVT_INFO2:            return "INFO2";
	case NEXRV_EXPORT_EVT_INFO3:            return "INFO3";
	case NEXRV_EXPORT_EVT_MEMREAD_0:        return "MEMREAD_0";
	case NEXRV_EXPORT_EVT_MEMREAD_1:        return "MEMREAD_1";
	case NEXRV_EXPORT_EVT_MEMREAD_2:        return "MEMREAD_2";
	case NEXRV_EXPORT_EVT_MEMREAD_4:        return "MEMREAD_4";
	case NEXRV_EXPORT_EVT_MEMREAD_8:        return "MEMREAD_8";
	case NEXRV_EXPORT_EVT_MEMWRITE_0:       return "MEMWRITE_0";
	case NEXRV_EXPORT_EVT_MEMWRITE_1:       return "MEMWRITE_1";
	case NEXRV_EXPORT_EVT_MEMWRITE_2:       return "MEMWRITE_2";
	case NEXRV_EXPORT_EVT_MEMWRITE_4:       return "MEMWRITE_4";
	case NEXRV_EXPORT_EVT_MEMWRITE_8:       return "MEMWRITE_8";
	case NEXRV_EXPORT_EVT_DAQ_DATA:         return "DAQ_DATA";
	case NEXRV_EXPORT_EVT_DAQ_COUNTER:      return "DAQ_COUNTER";
	case NEXRV_EXPORT_EVT_DAQ_LAST_PC:      return "DAQ_LAST_PC";
	default:                                return "INFO1";
  }
}

static unsigned char ctxp_type_to_bin(NexRvExportEventType t)
{
  // We always emit cycle_count for all events that have a _WITH_TIMESTAMP variant.
  // For OVERFLOW there is no timestamp variant in the v1 spec.
  switch (t)
  {
	case NEXRV_EXPORT_EVT_SYNC:             return 0x80; // SYNC_WITH_TIMESTAMP
	case NEXRV_EXPORT_EVT_BRANCH_TAKEN:     return 0x91; // BRANCH_TAKEN_WITH_TIMESTAMP
	case NEXRV_EXPORT_EVT_BRANCH_NOTTAKEN:  return 0x92; // BRANCH_NOTTAKEN_WITH_TIMESTAMP
	case NEXRV_EXPORT_EVT_INTERRUPT:        return 0x93; // INTERRUPT_WITH_TIMESTAMP
	case NEXRV_EXPORT_EVT_RFI:              return 0x95; // RFI_WITH_TIMESTAMP
	case NEXRV_EXPORT_EVT_CALL:             return 0x96; // CALL_WITH_TIMESTAMP
	case NEXRV_EXPORT_EVT_RETURN:           return 0x97; // RETURN_WITH_TIMESTAMP
	case NEXRV_EXPORT_EVT_CONTEXT:          return 0xC0; // CONTEXT_WITH_TIMESTAMP
	case NEXRV_EXPORT_EVT_WALLCLOCK:        return 0xC1; // WALLCLOCK_WITH_TIMESTAMP
	case NEXRV_EXPORT_EVT_INFO1:            return 0xF0; // INFO1_WITH_TIMESTAMP
	case NEXRV_EXPORT_EVT_INFO2:            return 0xF1; // INFO2_WITH_TIMESTAMP
	case NEXRV_EXPORT_EVT_INFO3:            return 0xF2; // INFO3_WITH_TIMESTAMP
	case NEXRV_EXPORT_EVT_OVERFLOW:         return 0x5F; // OVERFLOW (no timestamp variant)
	case NEXRV_EXPORT_EVT_MEMWRITE_0:       return 0xA0; // MEMWRITE_0_WITH_TIMESTAMP
	case NEXRV_EXPORT_EVT_MEMWRITE_1:       return 0xA1; // MEMWRITE_1_WITH_TIMESTAMP
	case NEXRV_EXPORT_EVT_MEMWRITE_2:       return 0xA2; // MEMWRITE_2_WITH_TIMESTAMP
	case NEXRV_EXPORT_EVT_MEMWRITE_4:       return 0xA4; // MEMWRITE_4_WITH_TIMESTAMP
	case NEXRV_EXPORT_EVT_MEMWRITE_8:       return 0xA8; // MEMWRITE_8_WITH_TIMESTAMP
	case NEXRV_EXPORT_EVT_MEMREAD_0:        return 0xB0; // MEMREAD_0_WITH_TIMESTAMP
	case NEXRV_EXPORT_EVT_MEMREAD_1:        return 0xB1; // MEMREAD_1_WITH_TIMESTAMP
	case NEXRV_EXPORT_EVT_MEMREAD_2:        return 0xB2; // MEMREAD_2_WITH_TIMESTAMP
	case NEXRV_EXPORT_EVT_MEMREAD_4:        return 0xB4; // MEMREAD_4_WITH_TIMESTAMP
	case NEXRV_EXPORT_EVT_MEMREAD_8:        return 0xB8; // MEMREAD_8_WITH_TIMESTAMP
	case NEXRV_EXPORT_EVT_DAQ_DATA:         return 0xE0; // DAQ_DATA_WITH_TIMESTAMP
	case NEXRV_EXPORT_EVT_DAQ_COUNTER:      return 0xE1; // DAQ_COUNTER_WITH_TIMESTAMP
	case NEXRV_EXPORT_EVT_DAQ_LAST_PC:      return 0xE2; // DAQ_LAST_PC_WITH_TIMESTAMP
	default:                                return 0xF0;
  }
}

// True for events whose `value1` is an address that may be "omitted"
// (value-only data capture): MEMREAD_1/2/4/8 and MEMWRITE_1/2/4/8.
static int ctxp_is_sized_mem(NexRvExportEventType t)
{
  switch (t)
  {
	case NEXRV_EXPORT_EVT_MEMREAD_1: case NEXRV_EXPORT_EVT_MEMREAD_2:
	case NEXRV_EXPORT_EVT_MEMREAD_4: case NEXRV_EXPORT_EVT_MEMREAD_8:
	case NEXRV_EXPORT_EVT_MEMWRITE_1: case NEXRV_EXPORT_EVT_MEMWRITE_2:
	case NEXRV_EXPORT_EVT_MEMWRITE_4: case NEXRV_EXPORT_EVT_MEMWRITE_8:
	  return 1;
	default:
	  return 0;
  }
}

// True for events that carry only `value2` (value1 column empty in text):
// SYNC, DAQ_DATA, DAQ_LAST_PC.
static int ctxp_value2_only(NexRvExportEventType t)
{
  return t == NEXRV_EXPORT_EVT_SYNC ||
		 t == NEXRV_EXPORT_EVT_DAQ_DATA ||
		 t == NEXRV_EXPORT_EVT_DAQ_LAST_PC;
}

// True for events that carry only `value1` (value2 column empty in text):
// MEMREAD_0, MEMWRITE_0 (address, no value).
static int ctxp_value1_only(NexRvExportEventType t)
{
  return t == NEXRV_EXPORT_EVT_MEMREAD_0 || t == NEXRV_EXPORT_EVT_MEMWRITE_0;
}

#pragma pack(push, 1)
typedef struct CtxpTraceEvent {
  unsigned char source_id;
  unsigned char type;
  unsigned long long value1;
  unsigned long long value2;
  unsigned long long cycle_count;
} CtxpTraceEvent;
#pragma pack(pop)

static void ctxp_write_text_event(FILE *f, const NexRvExportEvent *event)
{
  const char *type_str = ctxp_type_to_string(event->type);
  unsigned int sid = (unsigned int)event->source_id;

  // Colons are always present. For events that do not provide a value, keep field empty.
  // We always print cycle_count.
  if (event->type == NEXRV_EXPORT_EVT_OVERFLOW)
  {
	// No payloads
	fprintf(f, "#%u:%s:: @ %llu\n",
			sid, type_str,
			(unsigned long long)event->cycle_count);
  }
  else if (ctxp_value2_only(event->type))
  {
	// value1 empty, value2 carries the payload (SYNC target / DAQ tag / last PC)
	fprintf(f, "#%u:%s::0x%llx @ %llu\n",
			sid, type_str,
			(unsigned long long)event->value2,
			(unsigned long long)event->cycle_count);
  }
  else if (ctxp_value1_only(event->type))
  {
	// MEMREAD_0/MEMWRITE_0: address only, value column empty.
	fprintf(f, "#%u:%s:0x%llx: @ %llu\n",
			sid, type_str,
			(unsigned long long)event->value1,
			(unsigned long long)event->cycle_count);
  }
  else if (ctxp_is_sized_mem(event->type) && event->value1 == NEXRV_EXPORT_ADDR_OMITTED)
  {
	// Sized memory access with unknown address (value-only capture): value1 empty.
	fprintf(f, "#%u:%s::0x%llx @ %llu\n",
			sid, type_str,
			(unsigned long long)event->value2,
			(unsigned long long)event->cycle_count);
  }
  else
  {
	fprintf(f, "#%u:%s:0x%llx:0x%llx @ %llu\n",
			sid, type_str,
			(unsigned long long)event->value1,
			(unsigned long long)event->value2,
			(unsigned long long)event->cycle_count);
  }
}

static void ctxp_write_binary_event(FILE *f, const NexRvExportEvent *event)
{
  CtxpTraceEvent e;
  memset(&e, 0, sizeof(e));

  e.source_id = event->source_id;
  e.type = ctxp_type_to_bin(event->type);

  // Map payload semantics. value2-only events (SYNC, DAQ_DATA, DAQ_LAST_PC)
  // leave value1 unused (zero). Everything else passes value1/value2 through —
  // including the NEXRV_EXPORT_ADDR_OMITTED sentinel that the spec defines as
  // the binary "address omitted" marker for sized memory accesses.
  if (ctxp_value2_only(event->type))
  {
	e.value1 = 0;
	e.value2 = event->value2;
  }
  else
  {
	e.value1 = event->value1;
	e.value2 = event->value2;
  }

  // Only valid for types with MSB set. For OVERFLOW, parsers ignore the field.
  if (e.type & 0x80)
	e.cycle_count = event->cycle_count;
  else
	e.cycle_count = 0;

  // We have to enforce little-endian on multi-byte fields.
  // Write field-wise.
  (void)fwrite(&e.source_id, 1, 1, f);
  (void)fwrite(&e.type, 1, 1, f);
  write_u64_le(f, e.value1);
  write_u64_le(f, e.value2);
  write_u64_le(f, e.cycle_count);
}

static void ctxp_open_if_env_set(void)
{
  const char *bin_path = getenv("CTXP_TRACEFILE");
  const char *txt_path = getenv("CTXP_TEXT_TRACEFILE");

  if (bin_path != NULL)
  {
	g_ctxp_bin = fopen(bin_path, "wb");
	if (g_ctxp_bin != NULL)
	{
	  // Header: Magic 'CTXP' + header size 8 + version 1
	  (void)fwrite("CTXP", 1, 4, g_ctxp_bin);
	  write_u16_le(g_ctxp_bin, 8);
	  write_u16_le(g_ctxp_bin, 1);

	  // Metadata section: one entry #0="CPU0"
	  const char *name = "CPU0";
	  unsigned int name_len = (unsigned int)strlen(name);

	  // SectionLength includes SectionType+SectionLength itself.
	  // 2 + 2 + (1 + 1 + name_len)
	  unsigned int section_len = 4 + 2 + name_len;
	  write_u16_le(g_ctxp_bin, 0x0001);
	  write_u16_le(g_ctxp_bin, section_len);
	  (void)fputc(0, g_ctxp_bin);              // SourceId
	  (void)fputc((int)name_len, g_ctxp_bin);  // NameLen
	  (void)fwrite(name, 1, name_len, g_ctxp_bin);

	  printf("INFO: CTXP binary trace will be written to file '%s'.\n", bin_path);
	}
	else
	{
	  perror("Error opening CTXP_TRACEFILE");
	}
  }

  if (txt_path != NULL)
  {
	g_ctxp_txt = fopen(txt_path, "wt");
	if (g_ctxp_txt != NULL)
	{
	  // Header + metadata in text format
	  fprintf(g_ctxp_txt, "HDR:format=accemic//ctxp-txt,ver=1\n");
	  fprintf(g_ctxp_txt, "META:#0=\"CPU0\"\n");

	  printf("INFO: CTXP text trace will be written to file '%s'.\n", txt_path);
	}
	else
	{
	  perror("Error opening CTXP_TEXT_TRACEFILE");
	}
  }

  if (g_ctxp_bin != NULL || g_ctxp_txt != NULL)
  {
	(void)nexrv_export_register(ctxp_on_event, NULL);
  }
  else
  {
	// Not enabled.
  }
}

static void ctxp_close_files(void)
{
  if (g_ctxp_txt != NULL)
  {
	fclose(g_ctxp_txt);
	g_ctxp_txt = NULL;
	const char *txt_path = getenv("CTXP_TEXT_TRACEFILE");
	if (txt_path != NULL)
	  printf("INFO: CTXP text trace written to file '%s'.\n", txt_path);
  }
  if (g_ctxp_bin != NULL)
  {
	fclose(g_ctxp_bin);
	g_ctxp_bin = NULL;
	const char *bin_path = getenv("CTXP_TRACEFILE");
	if (bin_path != NULL)
	  printf("INFO: CTXP binary trace written to file '%s'.\n", bin_path);
  }
}

static void ctxp_on_event(const NexRvExportEvent *event, void *user_data)
{
  (void)user_data;
  if (event == NULL) return;

  if (g_ctxp_txt != NULL)
	ctxp_write_text_event(g_ctxp_txt, event);
  if (g_ctxp_bin != NULL)
	ctxp_write_binary_event(g_ctxp_bin, event);
}
