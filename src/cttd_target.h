// SPDX-FileCopyrightText: 2026 Accemic Technologies GmbH
// SPDX-License-Identifier: ISC
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

//****************************************************************************
// File cttd_target.h - Multi-target (multi-SRC) decode contexts
//
// A "target" is one trace source (one core / one encoder instance) in a
// funnel-merged Nexus stream, identified by the message SRC field. Each
// target owns the full per-source decoder state: PCInfo tables, PC/decode
// state, call stack, JTC/BP model state, timestamp accumulator, TCODE-58
// config-message tracking, and its own -pcout file.
//
// CLI model (-deco): `-target <src>` opens a target scope; the following
// -pcinfo/-pcout/-bp options bind to that scope. Config-message (TCODE 58)
// fields override only the emitting source's context; an explicitly given
// per-target CLI flag wins over that source's config message (same rule as
// single-target mode). The SRC field width itself stays stream-global
// (-src) -- without it no message of the merged stream is parseable at all
// (bootstrap, SPEC section 5).
//
// Without any -target option the decoder behaves exactly as before
// (single implicit context, no swap machinery in the decode path).

#ifndef CTTD_TARGET_H
#define CTTD_TARGET_H

#include <stdio.h>
#include "cttd.h"      // Nexus_TypeAddr
#include "cttd_info.h"  // InfoAddr

#define CTTD_TARGET_MAX      64  // SRC field is up to 6 bits
#define CTTD_CALLSTACK_MAX   32  // must match CALLSTACK_MAX (cttd.c)
#define CTTD_JTC_SIZE        64  // must match NEXDECO_JTC_SIZE (cttd_deco.c)
#define CTTD_BP_SIZE        512  // must match NEXDECO_BP_SIZE (cttd_deco.c)
#define CTTD_MSGFIELDS_MAX   10  // must match MSGFIELDS_MAX (cttd_deco.c)

// Per-target decoder state (mirror of the cttd_deco.c working statics).
typedef struct CttdDecoState
{
  // All four are addresses and therefore 64-bit (R1.2 / X2c): the snapshot
  // must not be narrower than the working statics in cttd_deco.c, or a
  // multi-target context switch would truncate an RV64 PC on save/restore.
  Nexus_TypeAddr pc;          // nexdeco_pc
  Nexus_TypeAddr lastAddr;    // nexdeco_lastAddr
  Nexus_TypeAddr branchSrc;   // nexdeco_branchSrc
  Nexus_TypeAddr stackRet;    // nexdeco_stackRet
  int nInstr;
  Nexus_TypeAddr jtcCache[CTTD_JTC_SIZE];
  unsigned char  jtcValid[CTTD_JTC_SIZE];
  unsigned char  bpTable[CTTD_BP_SIZE];
  int bpRemain;
  int bpFinalTaken;
  int resourceFullIcnt;       // resourceFull_ICNT
  int cfgReported;            // TCODE-58 first-seen flag (per source)
  unsigned long long tstampGlobal;
  int confBranchPredict;      // conf_BranchPredict (per-target effective)
  int confBranchPredictCli;   // conf_BranchPredictCli (CLI-wins rule per target)
  // DF address compression (P3, TCODE 13/14): XOR reference = the previous
  // data-trace message's FULL address. A separate valid flag (not a sentinel
  // value): data addresses are byte-granular, every 32/64-bit value is legal.
  Nexus_TypeAddr lastDaddr;   // nexdeco_lastDaddr
  int lastDaddrValid;         // nexdeco_lastDaddrValid
  int confDfXor;              // conf_DfXor (per-target effective)
  int confDfXorCli;           // conf_DfXorCli (CLI given; enable-only)
  // Address TEXT width (CAPS.23 = ADDR64). Per source for the same reason as
  // the two flags above, and with more consequence: a funnel-merged stream may
  // carry mixed XLEN (two RV32 cores plus one RV64 core is the trio build of
  // this repository), and a process-global flag let the RV64 source's config
  // message widen every other source's pcout. No CLI counterpart exists, so
  // there is no ...Cli twin -- the announcement is the only source.
  int confAddr64;             // cttd_conf_addr64 (per-target effective)
  // RepeatBranch replay context: the last handled message of THIS source
  // (in a merged stream the globally previous message may belong to a
  // different source, so the single-target msgFields[6..9] trick is wrong).
  unsigned long long lastMsg[CTTD_MSGFIELDS_MAX];
  int lastMsgPos;
  int lastMsgCnt;
  int lastMsgValid;
} CttdDecoState;

// Per-target call-stack state (mirror of the cttd.c working statics).
typedef struct CttdCallStackState
{
  Nexus_TypeAddr stack[CTTD_CALLSTACK_MAX];
  int top;
  int cnt;
} CttdCallStackState;

// Per-target PCInfo state (mirror of the cttd_info.c working statics;
// pointers are opaque here, ownership stays with cttd_info.c).
typedef struct CttdInfoState
{
  void *fInfo;
  InfoAddr prevAddr;
  int nInfoRec;
  int infoLast;
  void *pInfoRec;
  void *pInfoAddr;
  InfoAddr addrMin;
  InfoAddr addrMax;
  int sorted;                 // infoSorted (bisection precondition, per target)
} CttdInfoState;

typedef struct CttdTarget
{
  int used;                   // configured via -target
  const char *pcinfoPath;
  const char *pcoutPath;
  FILE *fOut;
  int bp;                     // -bp given inside this target scope
  CttdDecoState deco;
  CttdCallStackState cs;
  CttdInfoState info;
} CttdTarget;

// Registry (cttd.c)
extern CttdTarget cttd_targets[CTTD_TARGET_MAX];
extern int cttd_target_mode;    // != 0 once any -target was given
extern int cttd_target_active;  // -1 = none selected yet

// Orchestration (cttd.c). Switch saves the active target's state and
// restores the state of target 'src'. Returns 0 on success, -1 if 'src'
// is not a configured target (caller skips the message, warn-once).
extern int CttdTargetSwitch(unsigned int src);
// Active target's pcout (fallback: passed-through single-target FILE*).
extern FILE *CttdTargetActiveOut(FILE *fallback);
// Save the active target's state back to its slot (end of decode).
extern void CttdTargetFlushActive(void);

// Per-module state movers.
// cttd_deco.c:
extern void NexusDecoStateInit(CttdDecoState *s, int bp);
extern void NexusDecoStateSave(CttdDecoState *s);
extern void NexusDecoStateRestore(const CttdDecoState *s);
// cttd.c (call stack):
extern void CallStackStateInit(CttdCallStackState *s);
extern void CallStackStateSave(CttdCallStackState *s);
extern void CallStackStateRestore(const CttdCallStackState *s);
// cttd_info.c: detach moves the freshly loaded tables out of the working
// statics (so the next InfoInit starts empty); attach copies them back in.
extern void InfoDetach(CttdInfoState *s);
extern void InfoAttach(const CttdInfoState *s);
extern void InfoStateFree(CttdInfoState *s);

#endif // CTTD_TARGET_H
