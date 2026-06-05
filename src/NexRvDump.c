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
// File NexRvDump.c  - Nexus RISC-V Trace dumper

// Code below is written in plain C-code.
// It was compiled using VisualC, GNU and IAR C/C++ compiler.
//  1. Only standard C-types are used.
//  2. Only few standard C functions used - see notes with "#include <...>"
//  3. Only non K&R C is 'for (int x' and 'int x;' between instructions.

#include <stdio.h>  //  For NULL, 'printf', 'fopen, ...'
#include <stdlib.h> //  For 'exit'
#include <string.h> //  For 'strcmp', 'strchr' 
#include <ctype.h>  //  For 'isspace/isxdigit' etc.

#include "NexRv.h"    //  Common NEXUS_... #define (RISC-V specific subset)
#include "NexRvMsg.h" //  Definition of Nexus messages

// Decoder works on two files and dumper on first file
extern FILE *fNex; // Nexus messages (binary bytes)

static unsigned long long NexusFieldMask(int bits)
{
  if (bits >= 64) return ~0ULL;
  return (1ULL << bits) - 1ULL;
}

typedef struct NexusBitBuffer
{
  unsigned char *bytes;
  int bit_count;
  int capacity_bytes;
} NexusBitBuffer;

static void NexusBitBufferInit(NexusBitBuffer *buf)
{
  buf->bytes = NULL;
  buf->bit_count = 0;
  buf->capacity_bytes = 0;
}

static void NexusBitBufferTerm(NexusBitBuffer *buf)
{
  free(buf->bytes);
  buf->bytes = NULL;
  buf->bit_count = 0;
  buf->capacity_bytes = 0;
}

static int NexusBitBufferEnsure(NexusBitBuffer *buf, int bit_count)
{
  int needed_bytes = (bit_count + 7) / 8;
  if (needed_bytes <= buf->capacity_bytes) return 1;

  int new_capacity = buf->capacity_bytes > 0 ? buf->capacity_bytes : 8;
  while (new_capacity < needed_bytes) new_capacity *= 2;

  unsigned char *new_bytes = (unsigned char *)realloc(buf->bytes, (size_t)new_capacity);
  if (new_bytes == NULL) return 0;

  memset(new_bytes + buf->capacity_bytes, 0, (size_t)(new_capacity - buf->capacity_bytes));
  buf->bytes = new_bytes;
  buf->capacity_bytes = new_capacity;
  return 1;
}

static void NexusBitBufferReset(NexusBitBuffer *buf)
{
  int used_bytes = (buf->bit_count + 7) / 8;
  if (used_bytes > 0) memset(buf->bytes, 0, (size_t)used_bytes);
  buf->bit_count = 0;
}

static int NexusBitBufferAppend(NexusBitBuffer *buf, unsigned int value, int bits)
{
  if (bits <= 0) return 1;
  if (!NexusBitBufferEnsure(buf, buf->bit_count + bits)) return 0;

  for (int bit = 0; bit < bits; bit++)
  {
    if (value & (1u << bit))
    {
      int dst_bit = buf->bit_count + bit;
      buf->bytes[dst_bit / 8] |= (unsigned char)(1u << (dst_bit % 8));
    }
  }

  buf->bit_count += bits;
  return 1;
}

static unsigned long long NexusBitBufferPeekU64(const NexusBitBuffer *buf, int bits)
{
  unsigned long long value = 0;
  if (bits > 64) bits = 64;

  for (int bit = 0; bit < bits; bit++)
  {
    if (buf->bytes[bit / 8] & (unsigned char)(1u << (bit % 8)))
    {
      value |= 1ULL << bit;
    }
  }

  return value;
}

static void NexusBitBufferDiscardLowBits(NexusBitBuffer *buf, int bits)
{
  if (bits <= 0) return;
  if (bits >= buf->bit_count)
  {
    NexusBitBufferReset(buf);
    return;
  }

  int byte_shift = bits / 8;
  int bit_shift = bits % 8;
  int old_bytes = (buf->bit_count + 7) / 8;
  int new_bits = buf->bit_count - bits;
  int new_bytes = (new_bits + 7) / 8;

  for (int i = 0; i < new_bytes; i++)
  {
    unsigned int value = buf->bytes[i + byte_shift] >> bit_shift;
    if (bit_shift != 0 && (i + byte_shift + 1) < old_bytes)
    {
      value |= ((unsigned int)buf->bytes[i + byte_shift + 1]) << (8 - bit_shift);
    }
    buf->bytes[i] = (unsigned char)(value & 0xFFu);
  }

  if (old_bytes > new_bytes)
  {
    memset(buf->bytes + new_bytes, 0, (size_t)(old_bytes - new_bytes));
  }

  if ((new_bits & 7) != 0)
  {
    buf->bytes[new_bytes - 1] &= (unsigned char)((1u << (new_bits & 7)) - 1u);
  }

  buf->bit_count = new_bits;
}

static char *NexusBitBufferToHex(const NexusBitBuffer *buf)
{
  int nibble_count = (buf->bit_count + 3) / 4;
  if (nibble_count <= 0) nibble_count = 1;

  char *hex = (char *)malloc((size_t)nibble_count + 1u);
  if (hex == NULL) return NULL;

  int out = 0;
  int started = 0;
  for (int nibble = nibble_count - 1; nibble >= 0; nibble--)
  {
    unsigned int digit = 0;
    for (int bit = 0; bit < 4; bit++)
    {
      int src_bit = nibble * 4 + bit;
      if (src_bit >= buf->bit_count) continue;
      if (buf->bytes[src_bit / 8] & (unsigned char)(1u << (src_bit % 8)))
      {
        digit |= 1u << bit;
      }
    }

    if (!started && digit == 0 && nibble > 0) continue;
    hex[out++] = "0123456789abcdef"[digit];
    started = 1;
  }

  if (!started) hex[out++] = '0';
  hex[out] = '\0';
  return hex;
}

// Dump all Nexus messages (from 'fNex' file)
//  disp  - display options bit-mask (1-packets, 2-only TCODE+names, 4-summary)
int NexusDump(FILE *f, int disp)
{
  int fldDef  = -1;
  unsigned int currentTcode = 0;
  NexusBitBuffer fldBuf;

  int msgCnt    = 0;
  int msgBytes  = 0;
  int msgErrors = 0;
  int idleCnt   = 0;
  int srcPending = 0;

  NexusBitBufferInit(&fldBuf);

  unsigned char msgByte = 0;
  unsigned char prevByte = 0;
  for (;;)
  {
    prevByte = msgByte;
    if (fread(&msgByte, 1, 1, fNex) != 1) break;  // EOF

#if 1 // This will skip long sequnece of idles (visible in true captures ...)
    if (msgByte == 0xFF && prevByte == 0xFF)
    {
      idleCnt++;
      continue;
    }
#endif

    if (disp & 1)
    { 
      if (msgCnt > 0 && fldDef < 0)
      {
        fprintf(f, "\n");
      }
      fprintf(f, "0x%02X ", msgByte);
      for (int b = 0x80; b != 0; b >>= 1)
      {
        if (b == 0x2) fprintf(f, "_");
        if (msgByte & b) fprintf(f, "1"); else fprintf(f, "0");
      }
      fprintf(f, ":");
    }

    unsigned int mdo  = msgByte >> 2;
    unsigned int mseo = msgByte & 0x3;

    if (mseo == 0x2)
    {
      printf(" ERROR: At offset %d: MSEO='10' is not allowed\n", msgBytes + idleCnt);
      NexusBitBufferTerm(&fldBuf);
      return -1;  // Error return
    }

    if (fldDef < 0)
    {
      if (mseo == 0x3) 
      {
        if (disp & 1) fprintf(f, " IDLE\n");
        idleCnt++;
        continue;
      }

      if (mseo != 0x0)
      {
        printf(" ERROR: At offset %d: Message must start from MSEO='00'\n", msgBytes + idleCnt);
        NexusBitBufferTerm(&fldBuf);
        return -2;  // Error return
      }

      unsigned int tcode = mdo & NexusFieldMask(NEXUS_FLDSIZE_TCODE);
      for (int d = 0; nexusMsgDef[d].def != 0; d++)
      {
        if ((nexusMsgDef[d].def & 0x100) == 0) continue;
        if ((nexusMsgDef[d].def & 0xFF) == (int)tcode)
        {
          fldDef = d; // Found TCODE
          break;
        }
      }

      if (fldDef < 0)
      {
        printf(" ERROR: Message with TCODE=%d is not defined for RISC-V\n", tcode);
        NexusBitBufferTerm(&fldBuf);
        return -3;
      }

      currentTcode = tcode;
      NexusBitBufferReset(&fldBuf);
      msgBytes++;

      fldDef++;
      srcPending = (nexrv_conf_src_bits > 0);
      if (!NexusBitBufferAppend(&fldBuf, mdo >> NEXUS_FLDSIZE_TCODE, 6 - NEXUS_FLDSIZE_TCODE))
      {
        printf(" ERROR: Out of memory while accumulating message bits\n");
        NexusBitBufferTerm(&fldBuf);
        return -5;
      }

      if (disp & 3)
      {
        fprintf(f, " TCODE[6]=%d (MSG #%d) - %s", tcode, msgCnt, nexusMsgDef[fldDef - 1].name);
        if (!(disp & 1) || fldBuf.bit_count == 0) fprintf(f, "\n");
      }
      msgCnt++;

      if (tcode == NEXUS_TCODE_Error) msgErrors++;
    }
    else
    {
      // Accumulate 'mdo' to field value
      if (!NexusBitBufferAppend(&fldBuf, mdo, 6))
      {
        printf(" ERROR: Out of memory while accumulating message bits\n");
        NexusBitBufferTerm(&fldBuf);
        return -5;
      }
      msgBytes++;
    }

    // Extract SRC field (between TCODE and message-specific fields)
    if (srcPending && fldBuf.bit_count >= nexrv_conf_src_bits)
    {
      unsigned long long srcVal = NexusBitBufferPeekU64(&fldBuf, nexrv_conf_src_bits)
                                  & NexusFieldMask(nexrv_conf_src_bits);
      if (disp & 1) fprintf(f, " SRC[%d]=0x%llx", nexrv_conf_src_bits, srcVal);
      NexusBitBufferDiscardLowBits(&fldBuf, nexrv_conf_src_bits);
      srcPending = 0;
    }

    // Process fixed size fields (there may be more than one in one MDO record)
    while ((nexusMsgDef[fldDef].def & 0x200) && fldBuf.bit_count >= (nexusMsgDef[fldDef].def & 0xFF))
    {
      int fldSize = nexusMsgDef[fldDef].def & 0xFF;
      unsigned long long fldOut = NexusBitBufferPeekU64(&fldBuf, fldSize) & NexusFieldMask(fldSize);
      if (disp & 1) fprintf(f, " %s[%d]=0x%llx (%llu)", nexusMsgDef[fldDef].name, fldSize, fldOut, fldOut);
      fldDef++;
      NexusBitBufferDiscardLowBits(&fldBuf, fldSize);
    }

    if (mseo == 0x0)
    {
      if (disp & 1) fprintf(f, "\n");
      continue;
    }

    if (nexusMsgDef[fldDef].def & 0x400)
    {
      // Variable size field
      const char *fldName = nexusMsgDef[fldDef].name;
      char *fldHex = NexusBitBufferToHex(&fldBuf);
      if (fldHex == NULL)
      {
        printf(" ERROR: Out of memory while formatting message bits\n");
        NexusBitBufferTerm(&fldBuf);
        return -5;
      }

      if (disp & 1)
      {
        if (fldBuf.bit_count <= 64)
        {
          unsigned long long fldVal = NexusBitBufferPeekU64(&fldBuf, fldBuf.bit_count);
          fprintf(f, " %s[%d]=0x%s (%llu)\n", fldName, fldBuf.bit_count, fldHex, fldVal);
        }
        else
        {
          fprintf(f, " %s[%d]=0x%s\n", fldName, fldBuf.bit_count, fldHex);
        }
      }

      free(fldHex);

      if (mseo == 3)
      {
        fldDef = -1;
      }
      else
      {
        fldDef++;
      }
      NexusBitBufferReset(&fldBuf);
      continue;
    }

    if (fldBuf.bit_count > 0)
    {
      printf(" ERROR: Not enough bits for non-variable field\n");
      NexusBitBufferTerm(&fldBuf);
      return -4;
    }
  }

  if (disp & 4)
  {
    printf("\nStat: %d bytes, %d idles, %d messages, %d error messages", msgBytes, idleCnt, msgCnt, msgErrors);
    if (msgCnt > 0) printf(", %.2lf bytes/message", ((double)msgBytes) / msgCnt);
    printf("\n");
  }

  NexusBitBufferTerm(&fldBuf);
  return msgCnt; // Number of messages handled
}

//****************************************************************************
// End of NexRvDump.c file
