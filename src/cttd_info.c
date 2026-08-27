// SPDX-FileCopyrightText: 2020 IAR Systems AB
// SPDX-FileCopyrightText: 2026 Accemic Technologies GmbH
// SPDX-License-Identifier: ISC
/*
* Copyright (c) 2020 IAR Systems AB.
* Copyright (c) 2026 Accemic Technologies GmbH.
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
// File cttd_info.c - Nexus RISC-V instruction info access

// Code below is written in plain C-code.
// It was compiled using VisualC, GNU and IAR C/C++ compiler.
//  1. Only standard C-types are used.
//  2. Only few standard C functions used - see notes with "#include <...>"
//  3. Only non K&R C is 'for (int x' and 'int x;' between instructions.

#include <stdio.h>  //  For NULL, 'printf', 'fopen, ...'
#include <stdlib.h> //  For 'malloc', 'free'
#include <string.h> //  For 'strcmp', 'strchr' 
#include <ctype.h>  //  For 'isspace/isxdigit' etc.
#include <inttypes.h>   //  For scan formats SCNx64

#include "cttd_info.h"  //  Definition of Nexus messages
#include "cttd_target.h" // Multi-target state movers (InfoDetach/InfoAttach)

// int InfoParse(const char *t, InfoAddr *pAddr, unsigned int *pInfo, InfoAddr *pDest);

static FILE *fInfo = NULL;     // Instruction info (text-based records)

// "Rewind next time" sentinel for the streaming (no-table) lookup below:
// InfoGet() rewinds whenever addr <= prevAddr, so the sentinel must be
// GREATER-OR-EQUAL to every possible address. The historical 0xFFFFFFFF was
// exactly that for RV32, but on RV64 a Sv39 kernel address (0xFFFF_FFC0_...)
// is larger -- the rewind would silently not happen and the sequential scan
// would run off the end of the file. Behaviour for RV32 is unchanged (the
// comparison was, and stays, always true).
#define INFO_ADDR_REWIND  (~(InfoAddr)0)
static InfoAddr prevAddr = INFO_ADDR_REWIND;

typedef struct INFO_REC
{
  InfoAddr addr;          // Address of instruction
  InfoAddr dest;          // Destination address
  unsigned int  info;     // INFO for this instruction
  unsigned int _padding;  // Make it 64-bit aligned
} INFO_REC;

typedef struct INFO_ADDR
{
  unsigned int  info;     // INFO for this instruction
  InfoAddr      dest;     // Destination address
} INFO_ADDR;

static int nInfoRec = 0;
static int infoLast = 0;
static INFO_REC *pInfoRec   = NULL;
// Set at load time when the record table is STRICTLY ascending in address.
// Only then may InfoGet() bisect instead of scanning: on a strictly ascending
// table with unique keys both find exactly the same record, so the decode
// output is bit-for-bit the same -- with duplicates they could pick different
// records, hence the strict test. Matters on RV64: an Sv39 image splits the
// address space into a user and a kernel half, the dense table below is then
// far too large to build, and a linear scan across a multi-million-record
// table on every backward branch is not viable.
static int infoSorted = 0;

InfoAddr infoAddr_min = 0;
InfoAddr infoAddr_max = 0;
static INFO_ADDR *pInfoAddr  = NULL;

int InfoInit(const char *filename)
{
  prevAddr = INFO_ADDR_REWIND;
  fInfo = fopen(filename, "rt");
  if (fInfo == NULL) return -1; // Failed

  nInfoRec = 0;
  while (pInfoRec == NULL)      // Will run twice
  {
    if (nInfoRec > 0) // Allocate (second time ...)
    {
      pInfoRec = malloc(sizeof(INFO_REC) * nInfoRec);
    }

    nInfoRec = 0;
    fseek(fInfo, 0, SEEK_SET); // Rewind file
    char line[1000];
    while (fgets(line, sizeof(line), fInfo) != NULL)
    {
      if (line[0] == '.' && line[1] == 'e') break; // End
      if (line[0] == '.') continue; // Comment (ignore this line)
      if (line[0] == '\0' || line[0] == '\n') continue; // Ignore empty as well ...

      InfoAddr a, dest;
      unsigned int info;
      if (!InfoParse(line, &a, &info, &dest)) break;
      if (info == 0) break;

      if (pInfoRec != NULL)
      {
        pInfoRec[nInfoRec].addr = a;
        pInfoRec[nInfoRec].info = info;
        pInfoRec[nInfoRec].dest = dest;
        pInfoRec[nInfoRec]._padding = 0;
      }

      nInfoRec++;
    }

    if (nInfoRec == 0)
    {
      break;  // No records
    }
  }

  // Bisection precondition (see infoSorted): strictly ascending addresses.
  infoSorted = 0;
  if (pInfoRec != NULL && nInfoRec > 0)
  {
    infoSorted = 1;
    for (int i = 1; i < nInfoRec; i++)
    {
      if (pInfoRec[i].addr <= pInfoRec[i - 1].addr) { infoSorted = 0; break; }
    }
  }

#if 1 // Generate table with all INFO for all addresses
  if (pInfoRec != NULL)
  {
    // Get span of addresses ...
    infoAddr_min = pInfoRec[0].addr;
    infoAddr_max = pInfoRec[nInfoRec - 1].addr;

    // 64-bit note: the subtraction is unsigned, so an unsorted table (max <
    // min) wraps to a huge span and fails this test -- which is exactly the
    // wanted outcome, the dense table is only valid for one contiguous range.
    // On RV64 the realistic case is a genuinely huge span (Sv39 user/kernel
    // split); it lands in the same branch and the record lookup takes over.
    if ((infoAddr_max - infoAddr_min) <= 3 * ((InfoAddr)nInfoRec * 4))
    {
      printf("Cttd/Info: amin=0x%" PRIX64 ", amax=0x%" PRIX64 ", nRec=%d\n", infoAddr_min, infoAddr_max, nInfoRec);

      InfoAddr nDense = (infoAddr_max - infoAddr_min) + 1;

      // This is not so big (not more than 3x bigger than original).
      // Guard the size_t product AND the allocation itself: on RV64 the span
      // is bounded by the heuristic above but no longer by 2^32, and a failed
      // malloc used to be dereferenced right below (segfault instead of the
      // perfectly good record-table fallback).
      if (nDense <= (InfoAddr)(~(size_t)0) / sizeof(INFO_ADDR))
      {
        pInfoAddr = malloc(sizeof(INFO_ADDR) * (size_t)nDense);
      }

      if (pInfoAddr != NULL)
      {
        for (InfoAddr a = 0; a < nDense; a++)
        {
          pInfoAddr[a].info = 0;
          pInfoAddr[a].dest = 0;
        }

        for (int i = 0; i < nInfoRec; i++)
        {
          pInfoAddr[pInfoRec[i].addr - infoAddr_min].info = pInfoRec[i].info;
          pInfoAddr[pInfoRec[i].addr - infoAddr_min].dest = pInfoRec[i].dest;
        }
      }
      else
      {
        printf("Cttd/Info: dense table (%" PRIu64 " entries) not allocatable - using record lookup\n",
               (uint64_t)nDense);
      }
    }
  }
#endif

  infoLast = 0;
  return 0; // OK
}

void InfoTerm(void)
{
  if (fInfo != NULL) fclose(fInfo);
  fInfo = NULL;
  if (pInfoRec) free(pInfoRec);
  pInfoRec = NULL;
  if (pInfoAddr) free(pInfoAddr);
  pInfoAddr = NULL;
  nInfoRec = 0;
  infoLast = 0;
}

// Multi-target: move the working statics out into a per-target slot (so the
// next InfoInit starts from an empty state; ownership of the allocations
// moves to the slot) ...
void InfoDetach(CttdInfoState *s)
{
  s->fInfo = (void *)fInfo;
  s->prevAddr = prevAddr;
  s->nInfoRec = nInfoRec;
  s->infoLast = infoLast;
  s->pInfoRec = (void *)pInfoRec;
  s->pInfoAddr = (void *)pInfoAddr;
  s->addrMin = infoAddr_min;
  s->addrMax = infoAddr_max;
  s->sorted = infoSorted;

  fInfo = NULL;
  prevAddr = INFO_ADDR_REWIND;
  nInfoRec = 0;
  infoLast = 0;
  pInfoRec = NULL;
  pInfoAddr = NULL;
  infoAddr_min = 0;
  infoAddr_max = 0;
  infoSorted = 0;
}

// ... and copy a slot's state back into the working statics. The previously
// active state must have been detached to its own slot first.
void InfoAttach(const CttdInfoState *s)
{
  fInfo = (FILE *)s->fInfo;
  prevAddr = s->prevAddr;
  nInfoRec = s->nInfoRec;
  infoLast = s->infoLast;
  pInfoRec = (INFO_REC *)s->pInfoRec;
  pInfoAddr = (INFO_ADDR *)s->pInfoAddr;
  infoAddr_min = s->addrMin;
  infoAddr_max = s->addrMax;
  infoSorted = s->sorted;
}

void InfoStateFree(CttdInfoState *s)
{
  if (s->fInfo != NULL) fclose((FILE *)s->fInfo);
  s->fInfo = NULL;
  if (s->pInfoRec) free(s->pInfoRec);
  s->pInfoRec = NULL;
  if (s->pInfoAddr) free(s->pInfoAddr);
  s->pInfoAddr = NULL;
  s->nInfoRec = 0;
  s->infoLast = 0;
}

int InfoParse(const char *t, InfoAddr *pAddr, unsigned int *pInfo, InfoAddr *pDest)
{
  if (sscanf(t, "%" SCNx64, pAddr) != 1) return 0; // Syntax error

  t = strchr(t, ',');
  if (t == NULL) { *pInfo = 0; return 1; }
  t++;

  // We have <t><s>,<t>I<s>|<t>D<s> - convert text to bit-mask
  unsigned int info = 0;
  if (t[0] == 'L')  info = INFO_LINEAR;
  if (t[0] == 'B')  info = INFO_BRANCH;
  if (t[0] == 'J')  info = INFO_JUMP;
  if (t[0] == 'C')  info = INFO_JUMP | INFO_CALL;
  if (t[0] == 'R')  info = INFO_JUMP | INFO_INDIRECT | INFO_RET;

  if (info == 0) { *pInfo = 0; return 2; }; // At least one of above must be given

  if (t[1] == 'N')  info |= INFO_LINEAR;    // Non-executed (only BN)
  if (t[1] == 'I')  info |= INFO_INDIRECT;
  if (t[1] == '4')  info |= INFO_4;
  if (t[2] == '4')  info |= INFO_4;

  *pInfo = info;

  t = strchr(t, ','); // Destination addr is after second ','
  if (t != NULL && pDest != NULL)
  {
    if (sscanf(t + 1, "%" SCNx64, pDest) != 1) return 0; // Syntax error
  }
  return 3;
}

unsigned int InfoGet(InfoAddr addr, InfoAddr *pDest)
{
  if (pInfoAddr != NULL)
  {
    if (addr < infoAddr_min || addr > infoAddr_max)
    {
      return 0; // Incorrect address (no INFO)
    }
    *pDest = pInfoAddr[addr - infoAddr_min].dest;
    return pInfoAddr[addr - infoAddr_min].info;
  }

  if (pInfoRec != NULL)
  {
    if (infoSorted)
    {
      // Bisect (strictly ascending table, unique keys -- result-identical to
      // the scan below). Without this an RV64 image whose span is too large
      // for the dense table above costs O(nRec) per backward branch.
      int lo = 0, hi = nInfoRec - 1;
      while (lo <= hi)
      {
        int mid = lo + (hi - lo) / 2;   // no int overflow (nInfoRec is int)
        if (pInfoRec[mid].addr == addr)
        {
          infoLast = mid;
          if (pDest) *pDest = pInfoRec[mid].dest;
          return pInfoRec[mid].info;
        }
        if (pInfoRec[mid].addr < addr) lo = mid + 1; else hi = mid - 1;
      }
      return 0; // Not found ...
    }

    if (addr < pInfoRec[infoLast].addr)
    {
      // Look before ...
      for (int i = infoLast - 1; i >= 0; --i)
      {
        if (pInfoRec[i].addr == addr)
        {
          infoLast = i;
          if (pDest) *pDest = pInfoRec[i].dest;
          return pInfoRec[i].info;
        }
      }
    }
    else
    {
      // Look after ...
      for (int i = infoLast; i < nInfoRec; ++i)
      {
        if (pInfoRec[i].addr == addr)
        {
          infoLast = i;
          if (pDest) *pDest = pInfoRec[i].dest;
          return pInfoRec[i].info;
        }
      }
    }

    return 0; // Not found ...
  }

  if (fInfo != NULL)
  {
    if (addr <= prevAddr) fseek(fInfo, 0, SEEK_SET); // Rewind file (if not forward)
    prevAddr = addr;  // Save for next call (to avoid 'fseek')

    char line[1000];
    while (fgets(line, sizeof(line), fInfo) != NULL)
    {
      if (line[0] == '.' && line[1] == 'e') break; // End
      if (line[0] == '.') continue; // Comment (ignore this line)
      if (line[0] == '\0' || line[0] == '\n') continue; // Ignore empty as well ...

      InfoAddr a;
      unsigned int info;
      if (!InfoParse(line, &a, &info, pDest)) break;
      if (info == 0) break;
      if (a == addr) return info; // Found
    }
    prevAddr = INFO_ADDR_REWIND;
  }
  return 0; // Error (=0)
}

//****************************************************************************
// End of cttd_info.c file
