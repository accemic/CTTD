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
// File cttd_deco.c  - Nexus RISC-V Trace decoder reference implementation

// Code below is written in plain C-code.
// It was compiled using VisualC, GNU and IAR C/C++ compiler.
//  1. Only standard C-types are used.
//  2. Only few standard C functions used - see notes with "#include <...>"
//  3. Only non K&R C is 'for (int x' and 'int x;' between instructions.

#include <stdio.h>  //  For NULL, 'printf', 'fopen, ...'
#include <stdlib.h> //  For 'exit'
#include <string.h> //  For 'strcmp', 'strchr'
#include <ctype.h>  //  For 'isspace/isxdigit' etc.
#include <inttypes.h>   //  For print formats PRIx64

#include "cttd.h"    //  Common NEXUS_... #define (RISC-V specific subset)
#include "cttd_msg.h" //  Definition of Nexus messages
#include "cttd_info.h" //  Definition of Nexus messages
#include "cttd_export.h"
#include "cttd_target.h" // Multi-target (multi-SRC) decode contexts
#include "cttd_daq.h"  //  Shared DAQ (ACT-CAP) -> CTXP mapping

// Decoder works on two files and dumper on first file
extern FILE *fNex;      // Nexus messages (binary bytes)

#if 1 // Callstack related
extern int conf_CallStack;
extern int conf_CallStackStrict;   // -csstrict: mismatch is fatal again (W4)
extern void CallStack_Init();
extern void CallStack_Push(Nexus_TypeAddr ret);
extern Nexus_TypeAddr CallStack_Pop();
#endif

// Index of the message being handled, for the diagnostics of the fatal
// paths -- set by the main loop of NexusDeco.
unsigned int nexdeco_curMsgIdx = 0;

// Decode state is 64-bit throughout (R1.2 / X2c): on RV64 the reconstructed
// PC lives in the Sv39 kernel half (0xFFFF_FFC0_...), so every address-valued
// static must be Nexus_TypeAddr -- a 32-bit one truncated silently and the
// walk desynchronized at the first sync. nexdeco_src is NOT an address (SRC
// field, 0..63) and stays an int.
static Nexus_TypeAddr nexdeco_pc       = 1; // odd => unsynchronized until first sync message installs a PC
static Nexus_TypeAddr nexdeco_lastAddr = 1;
static unsigned int nexdeco_src        = 0; // Current message SRC field value
static int nInstr = 0;

// Address text formatting per the width contract in cttd.h: 8 hex digits
// while cttd_conf_addr64 == 0, 16 once the config message declared ADDR64.
// Rotating buffer so a single printf can render several addresses (the
// desync/call-stack diagnostics print up to three).
#define NEXDECO_ASTR_SLOTS 6
static const char *AStr(Nexus_TypeAddr a)
{
  static char buf[NEXDECO_ASTR_SLOTS][24];
  static int slot = 0;
  char *p = buf[slot];
  slot = (slot + 1) % NEXDECO_ASTR_SLOTS;
  snprintf(p, sizeof(buf[0]),
           cttd_conf_addr64 ? "0x%016" PRIx64 : "0x%08" PRIx64, (uint64_t)a);
  return p;
}

// DF address compression (P3, CTTE CT_EN_DF_ADDR_COMPRESS): with
// trTeDataAddrCompress = XOR the 5/6 DADDR field carries the XOR against the
// PREVIOUS data-trace message's full address; TCODE 13/14 are the
// synchronizing forms carrying the FULL address that (re-)seat this
// reference. Deliberately separate from nexdeco_lastAddr: the CF reference
// is PC-shifted and re-anchored by CF syncs, the DF reference is
// byte-granular and re-anchored ONLY by 13/14. The valid flag (no sentinel:
// any value is a legal data address) is cleared on a Nexus Error -- encoder
// symmetry: the formatter re-anchors with a 13/14 after every emitted ERROR.
static Nexus_TypeAddr nexdeco_lastDaddr = 0;
static int nexdeco_lastDaddrValid = 0;

// Additive instrumentation over upstream EmitICNT (control flow kept identical):
//  - nexdeco_branchSrc: PC of the last instruction the ICNT walk visited, i.e. the
//    branch SOURCE. The indirect-branch handlers read this instead of nexdeco_pc,
//    which the call-stack RET path transiently overwrites with the return TARGET.
//  - nexdeco_stackRet: last address popped from the call stack during the walk
//    (cttd.c CallStack_Pop() returns 1 as the empty-stack sentinel). Used to
//    cross-check the explicit (uncompressed) return target carried by UADDR.
static Nexus_TypeAddr nexdeco_branchSrc = 1;
static Nexus_TypeAddr nexdeco_stackRet = 1;

// Repeat count carried from a RepeatBranch message into the message it repeats,
// so the ResourceFull (RCODE=2) handler can emit a "RepeatHIST" verbose line
// (matches upstream 6cd138f).
static int dispHistRepeat = 0;

// Accemic VendorJTC (TCODE 57): 64-entry jump-target cache, bit-identical to
// the CTTE encoder model (ct_L2_msg_gen JtcCache): entry = full byte
// address; index = 6-bit XOR fold of the target address; updated by every
// plain BTYPE=0 IndirectBranchHist that carries a UADDR, read-only for
// VendorJTC messages. Statics start invalid; Cttd decodes one capture per
// process, matching the encoder's per-session cache lifetime (the encoder
// additionally clears its cache on trace-off AND on a FIFO-overrun recovery
// sync, see JtcInit below).
#define NEXDECO_JTC_SIZE 64
_Static_assert(NEXDECO_JTC_SIZE == CTTD_JTC_SIZE, "cttd_target.h mirror out of sync");
static Nexus_TypeAddr nexdeco_jtcCache[NEXDECO_JTC_SIZE];
static unsigned char  nexdeco_jtcValid[NEXDECO_JTC_SIZE];

// Accemic JTC: drop the whole cache. Called on a FIFO-overrun recovery sync
// (SYNC=7) -- exactly where the encoder clears JtcValid (ct_L2_msg_gen.sv,
// FIFO_OVERRUN arm). Without the pair, an INSTALL that fell into the dropped
// window is unknown here while the encoder still counts on it: the next HIT on
// that index either finds an empty slot ("index N not yet installed") or a
// stale one that resolves to a non-indirect instruction. Found on the KV260
// under sustained saturation, reproduced by tests/overflow/03_jtc_overflow.
static void JtcInit(void)
{
  for (int i = 0; i < NEXDECO_JTC_SIZE; i++) { nexdeco_jtcCache[i] = 0; nexdeco_jtcValid[i] = 0; }
}

// ENCODER MIRROR -- must fold exactly like ct_L2_msg_gen.sv jtc_fold()
// (CTTE commit `1010b424`, decision E-R-2): XOR of 6-bit address slices starting
// at bit 2, up to JTC_FOLD_MSB. That bound is 25 while the stream is 32-bit
// (the historical four slices [7:2] [13:8] [19:14] [25:20] -- bit-identical,
// hence the 32-bit output does not move) and 63 once CAPS.23 announced
// ADDR64. Without the extension the index would be blind above bit 25, and
// under Sv39 the kernel/user split lives entirely up there: every kernel
// target would hash as if its high half were zero and collide with the user
// target sharing its low 24 bits. The top slice is short (bits 63:62) and
// enters zero-extended, exactly as the RTL loop does it.
// A one-bit disagreement desynchronizes the two caches SILENTLY, so this is
// written as the same loop rather than as a hand-unrolled XOR chain.
// Note the dependency: a 64-bit stream MUST carry the config message, or the
// decoder folds with the 32-bit bound while the encoder used the 64-bit one.
#define NEXDECO_JTC_FOLD_MSB (cttd_conf_addr64 ? 63 : 25)
static unsigned int JtcIndex(Nexus_TypeAddr a)
{
  const int msb = NEXDECO_JTC_FOLD_MSB;
  unsigned int h = 0;
  for (int i = 2; i <= msb; i += 6)
  {
    unsigned int sl = 0;
    for (int b = 0; b < 6; b++)
      if ((i + b) <= msb) sl |= (unsigned int)((a >> (i + b)) & 1ULL) << b;
    h ^= sl;
  }
  return h & 0x3F;
}

// Accemic VendorBP (TCODE 56, enabled with -bp): branch-prediction model,
// bit-identical to the CTTE encoder (ct_L2_msg_gen PredTable): 2^9
// direct-mapped 2-bit saturating counters, index = (pc>>2)&0x1FF, init
// weakly-not-taken (01), updated with the ACTUAL outcome of every direct
// branch this decoder resolves -- whether resolved from the predictor
// itself (the normal BP-mode case), from a pending HIST bit (the encoder's
// sync-seed path), or from DirectBranch(Sync) final-taken semantics. In BP
// mode every branch the encoder processed is walked here exactly once and
// in program order, so both models stay in lockstep. Reset at decode start,
// on a Nexus Error, on trace-off (correlation) and on a FIFO-overrun
// recovery sync -- mirroring the encoder's clear points.
extern int conf_BranchPredict; // cttd.c (-bp with -deco)
extern int conf_BranchPredictCli;    // cttd.c: -bp given explicitly (wins over TCODE-58 autoconfig)
extern int cttd_conf_src_bits_cli;  // cttd.c: -src given explicitly (wins over TCODE-58 autoconfig)
extern int cttd_conf_src_bits;      // cttd.c: SRC field width (autoconfig target)
// DF XOR mode sources, all ENABLE-only (there is no disable path, mirroring
// the BP rule where ENAB.5=0 never turns an earlier auto-enable off).
// Priority/coexistence: (1) CLI -dfxor (needed for data-only streams, which
// carry NO config message -- the emission hangs on the CF trace start);
// (2) TCODE-58 autoconfig from ENAB bit 21 (DF_ADDR_COMPRESS); (3) stream
// evidence: a decoded 13/14 implies the encoder ran compression (it never
// emits 13/14 in mode FULL), so the handler enables the mode itself -- this
// closes the data-only gap without any flag, because the T2 re-anchor
// contract guarantees a 13/14 precedes the first XOR'd 5/6.
extern int conf_DfXor;      // cttd.c (-dfxor with -deco, or auto)
extern int conf_DfXorCli;   // cttd.c: -dfxor given explicitly

#define NEXDECO_BP_SIZE 512
_Static_assert(NEXDECO_BP_SIZE == CTTD_BP_SIZE, "cttd_target.h mirror out of sync");
static unsigned char nexdeco_bpTable[NEXDECO_BP_SIZE];
static int nexdeco_bpRemain = 0;     // pending VendorBP walk: branches left to
                                     // resolve; the last one inverts the prediction
static int nexdeco_bpFinalTaken = 0; // DirectBranch(Sync) semantics: the walk's
                                     // final branch is taken by message contract

static void BpInit(void)
{
  for (int i = 0; i < NEXDECO_BP_SIZE; i++) nexdeco_bpTable[i] = 1;
  nexdeco_bpRemain = 0;
  nexdeco_bpFinalTaken = 0;
}

static unsigned int BpIndex(Nexus_TypeAddr pc) { return (unsigned int)(pc >> 2) & (NEXDECO_BP_SIZE - 1); }

static int BpPredict(Nexus_TypeAddr pc) { return nexdeco_bpTable[BpIndex(pc)] >= 2; }

static void BpUpdate(Nexus_TypeAddr pc, int taken)
{
  unsigned int i = BpIndex(pc);
  if (taken) { if (nexdeco_bpTable[i] < 3) nexdeco_bpTable[i]++; }
  else       { if (nexdeco_bpTable[i] > 0) nexdeco_bpTable[i]--; }
}

// ICNT adjustment carried across ResourceFull messages (positive or negative).
// RCODE=0 (ICNT overflow) adds RDATA here; RCODE=1 (HIST overflow) subtracts the
// halfwords just walked while resolving the overflowed history. The next
// regular packet's ICNT is corrected by this amount on entry to EmitICNT.
static int resourceFull_ICNT = 0;

// Per RISC-V N-Trace spec 8.5 (Timestamp Reporting): synchronizing messages
// carry an absolute TSTAMP (handled in each *Sync case as `tstampGlobal = TSTAMP`),
// while all other message types report TSTAMP as a relative offset from the last
// reported timestamp — hence the unconditional accumulation at every non-sync site.
unsigned long long tstampGlobal = 0;

// TCODE-58 config message first-seen flag. File-static (not function-local)
// so multi-target mode can keep it per source.
static int nexdeco_cfgReported = 0;

// Multi-target: last handled message of the ACTIVE source, for RepeatBranch
// replay. In a funnel-merged stream the globally previous message may belong
// to a different source, so the single-target msgFields[6..9] trick cannot be
// used; this working copy is swapped with the per-target context.
static unsigned long long nexdeco_lastMsg[CTTD_MSGFIELDS_MAX];
static int nexdeco_lastMsgPos = 0;
static int nexdeco_lastMsgCnt = 0;
static int nexdeco_lastMsgValid = 0;

// Multi-target state movers (cttd_target.h). The working statics above and
// the JTC/BP/ICNT/timestamp state further up are the "active" register set;
// switching targets swaps them against the per-target context structs.
void NexusDecoStateInit(CttdDecoState *s, int bp)
{
  s->pc = 1;
  s->lastAddr = 1;
  s->branchSrc = 1;
  s->stackRet = 1;
  s->nInstr = 0;
  for (int i = 0; i < CTTD_JTC_SIZE; i++) { s->jtcCache[i] = 0; s->jtcValid[i] = 0; }
  for (int i = 0; i < CTTD_BP_SIZE; i++) s->bpTable[i] = 1; // weakly-not-taken
  s->bpRemain = 0;
  s->bpFinalTaken = 0;
  s->resourceFullIcnt = 0;
  s->cfgReported = 0;
  s->tstampGlobal = 0;
  s->confBranchPredict = bp;
  s->confBranchPredictCli = bp;
  s->lastDaddr = 0;
  s->lastDaddrValid = 0;
  s->confDfXor = conf_DfXorCli;     // a global -dfxor applies to every target
  s->confDfXorCli = conf_DfXorCli;
  s->confAddr64 = 0;                // no CLI path: only this source's CAPS.23
  for (int i = 0; i < CTTD_MSGFIELDS_MAX; i++) s->lastMsg[i] = 0;
  s->lastMsgPos = 0;
  s->lastMsgCnt = 0;
  s->lastMsgValid = 0;
}

void NexusDecoStateSave(CttdDecoState *s)
{
  s->pc = nexdeco_pc;
  s->lastAddr = nexdeco_lastAddr;
  s->branchSrc = nexdeco_branchSrc;
  s->stackRet = nexdeco_stackRet;
  s->nInstr = nInstr;
  for (int i = 0; i < CTTD_JTC_SIZE; i++) { s->jtcCache[i] = nexdeco_jtcCache[i]; s->jtcValid[i] = nexdeco_jtcValid[i]; }
  for (int i = 0; i < CTTD_BP_SIZE; i++) s->bpTable[i] = nexdeco_bpTable[i];
  s->bpRemain = nexdeco_bpRemain;
  s->bpFinalTaken = nexdeco_bpFinalTaken;
  s->resourceFullIcnt = resourceFull_ICNT;
  s->cfgReported = nexdeco_cfgReported;
  s->tstampGlobal = tstampGlobal;
  s->confBranchPredict = conf_BranchPredict;
  s->confBranchPredictCli = conf_BranchPredictCli;
  s->lastDaddr = nexdeco_lastDaddr;
  s->lastDaddrValid = nexdeco_lastDaddrValid;
  s->confDfXor = conf_DfXor;
  s->confDfXorCli = conf_DfXorCli;
  s->confAddr64 = cttd_conf_addr64;
  for (int i = 0; i < CTTD_MSGFIELDS_MAX; i++) s->lastMsg[i] = nexdeco_lastMsg[i];
  s->lastMsgPos = nexdeco_lastMsgPos;
  s->lastMsgCnt = nexdeco_lastMsgCnt;
  s->lastMsgValid = nexdeco_lastMsgValid;
}

void NexusDecoStateRestore(const CttdDecoState *s)
{
  nexdeco_pc = s->pc;
  nexdeco_lastAddr = s->lastAddr;
  nexdeco_branchSrc = s->branchSrc;
  nexdeco_stackRet = s->stackRet;
  nInstr = s->nInstr;
  for (int i = 0; i < CTTD_JTC_SIZE; i++) { nexdeco_jtcCache[i] = s->jtcCache[i]; nexdeco_jtcValid[i] = s->jtcValid[i]; }
  for (int i = 0; i < CTTD_BP_SIZE; i++) nexdeco_bpTable[i] = s->bpTable[i];
  nexdeco_bpRemain = s->bpRemain;
  nexdeco_bpFinalTaken = s->bpFinalTaken;
  resourceFull_ICNT = s->resourceFullIcnt;
  nexdeco_cfgReported = s->cfgReported;
  tstampGlobal = s->tstampGlobal;
  conf_BranchPredict = s->confBranchPredict;
  conf_BranchPredictCli = s->confBranchPredictCli;
  nexdeco_lastDaddr = s->lastDaddr;
  nexdeco_lastDaddrValid = s->lastDaddrValid;
  conf_DfXor = s->confDfXor;
  conf_DfXorCli = s->confDfXorCli;
  cttd_conf_addr64 = s->confAddr64;
  for (int i = 0; i < CTTD_MSGFIELDS_MAX; i++) nexdeco_lastMsg[i] = s->lastMsg[i];
  nexdeco_lastMsgPos = s->lastMsgPos;
  nexdeco_lastMsgCnt = s->lastMsgCnt;
  nexdeco_lastMsgValid = s->lastMsgValid;
}

// W4: a RETURN whose target the trace carries EXPLICITLY, but which the
// return-address mirror predicted differently.
//
// WHY THIS IS NOT AN ERROR. The mirror has exactly one job that affects the
// reconstructed PC: implicit-return compression. There the encoder emits NO
// message for the return and the walk continues from the popped address
// (EmitICNT, "We should continue from PC we just pop from the stack") -- the
// mirror IS the address source, and an empty mirror is a hard error. In every
// other case the target comes from the stream: IndirectBranch(Hist) XOR the
// UADDR into nexdeco_lastAddr, VendorJTC reads the cache, the syncs load FADDR.
// The popped value never reaches nexdeco_lastAddr; it only lands in
// nexdeco_stackRet for this comparison. So on an explicit return the decode is
// fully determined WITHOUT the mirror and the check is pure redundancy.
//
// And the mismatch is expected, not exotic: the CTTE encoder folds a return
// only when its OWN stack predicted the target (ct_L23_preproc_composer_etip.sv
// compares pending_ret_target against the actual next_iaddr; the reference
// encoder in cttd_enco.c uses the same `checkRetNext == addr` rule). A return
// the hardware stack cannot predict is therefore ALWAYS transmitted explicitly
// -- which is precisely the message we are looking at. Linux hits this at every
// scheduler switch: `schedule` is entered from task A and returns into task B
// (KV260 cv64a6 boot, W1: ret at 0xffffffff8096881e, mirror 0xffffffff8059686a,
// trace 0xffffffff8096dbc6). Killing the decode there means no preemptively
// multitasking kernel can be decoded past its first context switch.
//
// WHY THE MIRROR IS NOT CLEARED HERE. Both models pop on EVERY return,
// predicted or not (RTL: `if (ret_sp_next > 0) ... ret_sp_next - 1`). The
// decoder mirror stays a faithful copy of the encoder stack only if it performs
// exactly the same pushes and pops. Clearing it "to re-anchor" would desync it
// from hardware, and the next folded return -- which the encoder emitted
// implicitly precisely because ITS stack was right -- would hit an empty mirror
// ("Not enough entires on callstack"). The re-anchor that is both needed and
// sufficient happens on the PC: the caller has already installed the explicit
// trace target. So: report, count, continue.
#define NEXDECO_CS_REPORT_MAX 8
static unsigned int nexdeco_csReanchor = 0;   // process-wide (all sources)
static void CallStackReanchor(Nexus_TypeAddr src, Nexus_TypeAddr predicted,
                              Nexus_TypeAddr actual)
{
  nexdeco_csReanchor++;
  if (conf_CallStackStrict)
  {
    // Historic behaviour, opt-in via -csstrict: for a single-task program the
    // mirror cannot legitimately miss, so the mismatch is a desync signal.
    printf("Error: return at %s: call-stack target %s != trace target %s.\n",
           AStr(src), AStr(predicted), AStr(actual));
    exit(EXIT_FAILURE);
  }
  if (nexdeco_csReanchor <= NEXDECO_CS_REPORT_MAX)
  {
    printf("INFO: call-stack re-anchor at MSG #%u: return at %s -- mirror predicted %s, "
           "trace target is %s (explicit target wins: context switch / longjmp / "
           "mirror overrun)\n",
           nexdeco_curMsgIdx, AStr(src), AStr(predicted), AStr(actual));
    if (nexdeco_csReanchor == NEXDECO_CS_REPORT_MAX)
      printf("INFO: further call-stack re-anchors are only counted (see the Stat line)\n");
  }
}

static int EmitErrorMsg(const char *err)
{
  printf("\nERROR: %s\n", err);
  return -10;
}

static void EmitSyncPC(FILE *f, Nexus_TypeAddr pc)
{
  // Do not write sync address to pcout — it will be emitted by the
  // subsequent message's EmitICNT when it walks over this address.
  (void)f;
  printf("SYNC PC: %s\n", AStr(pc));
}

static unsigned long long NexusFieldMask(int bits)
{
  if (bits >= 64) return ~0ULL;
  return (1ULL << bits) - 1ULL;
}

// This function is called with -1 parameter to reach next BRANCH.
// Otherwise it is 'n' 16-bit steps (over direct JUMP/CALL as well).
// It should never step over INDIRECT instruction (RET or JUMP/CALL)
// -> Updates "nexdeco_pc"
static int EmitICNT(FILE *f, int n, unsigned long long hist, int disp, unsigned long long tstamp)
{
  if (disp & 1) printf(". next_iaddr=%s, EmitICNT(n=%d,hist=0x%llx)\n", AStr(nexdeco_pc), n, hist);

  // Do not emit speculative PCs before the first synchronization message
  // establishes a valid decoder starting address.
  if (nexdeco_pc & 1) return 0;

  int doneICNT = 0; // Halfwords actually walked; returned so the ResourceFull
                    // handler can adjust the cross-message accumulator.

  // Apply any pending adjustment from ResourceFull messages since the last
  // regular packet. The adjustment may be positive (ICNT overflow from the
  // encoder, RCODE=0) or negative (halfwords already walked while resolving
  // a HIST overflow, RCODE=1).
  if (n >= 0 && resourceFull_ICNT != 0)
  {
    if (disp & 1) printf(". ICNT adjust: %d to %d\n", n, n + resourceFull_ICNT);

    n += resourceFull_ICNT;
    if (n < 0) return EmitErrorMsg("ICNT adjustment ERROR");

    resourceFull_ICNT = 0;    // Make adjustment 'consumed'

  }

  Nexus_TypeHist histMask = 0;  // MSB is first in history, so we need sliding mask
  if (hist != 0)
  {
    // Mirrors upstream NexRv (refcode 2025/01/02): with the MSB 'stop-bit' of a
    // full-width history set, the sliding mask below overflows to 0 and the
    // loop never ends (hit by examples/t1 with every -cs/-rpt configuration).
    if (hist & (((Nexus_TypeHist)1u) << (sizeof(Nexus_TypeHist) * 8 - 1)))  // Is MSB 'stop-bit' set?
    {
      histMask = (((Nexus_TypeHist)1u) << (sizeof(Nexus_TypeHist) * 8 - 2)); // All 31/63-bits of history are valid
    }
    else
    {
      histMask = 0x1; while (histMask <= hist) histMask <<= 1; histMask >>= 2;
    }
  }

  int walkGuard = 0;
  while (n != 0)
  {
    // Robustness (KV260 campaign rob_syncfix, 2026-08-01): a walk-to-branch
    // (n<0) that runs into a branch-free self-loop (e.g. crt0's
    // `_exit: j _exit`) NEVER terminates -- the decoder hung for 55 min in a
    // 400-kB capture. A correct encoder window is orders of magnitude
    // smaller; anything beyond the bound is a broken or hostile stream.
    if (++walkGuard > 20000000)
      return EmitErrorMsg("ICNT walk did not terminate (branchless self-loop?)");
    Nexus_TypeAddr origin_pc = nexdeco_pc;
    nexdeco_branchSrc = nexdeco_pc; // Track the source PC; after the loop this
                                    // holds the terminal (branch) instruction.

    fprintf(f, "%s", AStr(nexdeco_pc));
    printf("%d PC: %s \n", nInstr, AStr(nexdeco_pc));
    nInstr++; // Statistics (for compression display)

    InfoAddr a;
    unsigned int info = InfoGet((InfoAddr)nexdeco_pc, &a);
    if (info == 0)
    {
      printf("EmitErrorMsg: info == 0 @%s\n", AStr(nexdeco_pc));
      return EmitErrorMsg("No entry in -pcinfo found.");
    }

    // Track halfwords consumed in this call (returned to caller).
    if (info & INFO_4) doneICNT += 2; else doneICNT += 1;

    if (disp & 0x10) // Append type of instruction to plain PC value
    {
      int nt = 0;
      char t[8];

      if (info & INFO_CALL)         t[nt++] = 'C';
      else if (info & INFO_RET)     t[nt++] = 'R';
      else if (info & INFO_JUMP)    t[nt++] = 'J';
      else if (info & INFO_BRANCH)  t[nt++] = 'B';
      else                          t[nt++] = 'L';

      // Report non-taken branch as BN
      if (info & INFO_BRANCH)
      {
        // This is a little more demanding as it is different if we have history messages or not
        if (hist == 0)
        {
          if (n > (int)((info & INFO_4) + 2))  // Tricky: '(info & INFO_4)' is either 2 or 0
          {
            t[nt++] = 'N';  // Only last branch is considered taken - all before are not
          }
        }
        else if (!(histMask & hist))
        {
          t[nt++] = 'N';  // Not taken branch in history
        }
      }

      if (info & (INFO_INDIRECT) && !(info & INFO_RET)) t[nt++] = 'I';
      if (info & INFO_4) t[nt++] = '4'; else t[nt++] = '2';
      t[nt] = '\0';
      fprintf(f, ",%s", t);
    }
    fprintf(f, "\n");

    if (n > 0)
    {
      // Step over 16-bit unit
      if (info & INFO_4) n -= 2; else n -= 1;
      if (n < 0) return EmitErrorMsg("ICNT too small");
    }

    if (conf_CallStack > 0 && (info & INFO_CALL))
    {
      // This is call (direct or indirect) push address after '[c]jal[r]) to the stack
      Nexus_TypeAddr ret = nexdeco_pc + ((info & INFO_4) ? 4 : 2);
      CallStack_Push(ret);
    }

    if (info & INFO_INDIRECT) // Cannot continue over indirect...
    {
      // We always pop the stack if we see RET ...
      if (conf_CallStack > 0 && (info & INFO_RET))
      {
        Nexus_TypeAddr ret = CallStack_Pop();
        nexdeco_stackRet = ret; // Remember the call-stack prediction so the
                                // indirect handler can verify it against UADDR.
        // nexdeco_addrCheck = ret;  // Set PC to be checked (next time)
        if (conf_CallStack > 0)
        {
          nexdeco_pc = ret;
        }
        if (n != 0)
        {
          // We should continue from PC we just pop from the stack 
          if (ret == 1)
          {
            return EmitErrorMsg("Not enough entires on callstack");
          }
          continue;
        }
      }

      if (n > 0) {
        printf("ERROR EmitICNT with n=%d\n", n);
        return EmitErrorMsg("indirect address encountered in ICNT");
      }
      break;
    }

    if (info & INFO_BRANCH)
    {
      if (conf_BranchPredict && histMask == 0)
      {
        // Accemic BP mode, no pending history bit for this branch (the
        // encoder emits no HIST in BP mode; only sync-seed bits appear and
        // are consumed by the hist path below). Resolve from the predictor:
        //  - inside a VendorBP walk (bpRemain > 0) the LAST branch inverts
        //    the prediction (that is what the message reports);
        //  - a DirectBranch(Sync) walk's final branch is taken by message
        //    contract (bpFinalTaken; the encoder updates its model on that
        //    sync-carried branch too);
        //  - every other branch was correctly predicted, or a VendorBP
        //    message would have preceded this walk.
        // The predictor is updated with the actual outcome in all cases.
        int bpTaken;
        if (nexdeco_bpFinalTaken && n == 0)
        {
          bpTaken = 1;
        }
        else
        {
          bpTaken = BpPredict(nexdeco_pc);
          if (nexdeco_bpRemain > 0)
          {
            if (nexdeco_bpRemain == 1) bpTaken = !bpTaken; // the mispredicted one
            nexdeco_bpRemain--;
            if (nexdeco_bpRemain == 0 && n < 0)
            {
              n = 0; // VendorBP walk ends right after the mispredicted branch
            }
          }
        }
        if (disp & 1) printf(". BPwalk pc=%s predict=%d bpRemain(after)=%d taken=%d n=%d\n",
                             AStr(nexdeco_pc), BpPredict(nexdeco_pc), nexdeco_bpRemain, bpTaken, n);
        BpUpdate(nexdeco_pc, bpTaken);
        if (bpTaken) info |= INFO_JUMP;
        else         info |= INFO_NOJUMP;
      }
      else if (hist == 0)
      {
        // This is calling as DirectBranch
        if (n == 0)
        {
          info |= INFO_JUMP;  // Force PC change below
        }
	else {
		info |= INFO_NOJUMP;
	}
      }
      else
      {
        if (histMask & hist)
        {
          info |= INFO_JUMP;  // Force PC change below
        }
        else {
          // NOT-TAKEN branch ...
          info |= INFO_NOJUMP;
        }

        // Accemic BP mode: a branch resolved from a pending HIST bit (the
        // encoder's sync-seed path) updates the predictor too -- the
        // encoder updated its model when it processed that branch.
        if (conf_BranchPredict) BpUpdate(nexdeco_pc, (histMask & hist) != 0);

        // Consume history bit ...
        histMask >>= 1;
        if (histMask == 0 && n < 0)
        {
          n = 0;  // This will stop the loop (when we were called with HIST only)
        }
      }
    }

    if (info & INFO_JUMP)   nexdeco_pc = a;   // Direct jump/call/branch
    else if (info & INFO_4) nexdeco_pc += 4;  // Linear 4 or 2 otherwise
    else                    nexdeco_pc += 2;

    // Emit export event for control-flow changes.
    // Note: We keep the legacy behavior: only branch/jump/call/return edges are emitted,
    // not linear instruction flow.
    if ((info & INFO_NOJUMP) || (info & INFO_JUMP))
    {
      CttdExportEventType evt = CTTD_EXPORT_EVT_BRANCH_TAKEN;
      const char *cmt = NULL;

      if (info & INFO_CALL) {
        evt = CTTD_EXPORT_EVT_CALL;
      } else if (info & INFO_RET) {
        evt = CTTD_EXPORT_EVT_RETURN;
      } else if (info & INFO_NOJUMP) {
        evt = CTTD_EXPORT_EVT_BRANCH_NOTTAKEN;
        cmt = "no-jump";
      } else {
        evt = CTTD_EXPORT_EVT_BRANCH_TAKEN;
        cmt = "jump";
      }

      cttd_export_emit_cf(nexdeco_src, evt, origin_pc, nexdeco_pc, tstamp, cmt);
    }
  }

  // Check if still hist-bits pending ...
  if (histMask != 0) {
    printf("WARNING: hist bits pending\n");
    fprintf(f, "WARNING: hist bits pending\n");
  }

  return doneICNT;
}

#define MSGFIELDS_MAX   10 // Some reasonable limit
_Static_assert(MSGFIELDS_MAX == CTTD_MSGFIELDS_MAX, "cttd_target.h mirror out of sync");

static int          msgFieldPos = 0;
static unsigned long long msgFields[MSGFIELDS_MAX];
static int          msgFieldCnt = 0;

// Wide mirror of the field accumulator (fldVal/fldBits) for fields that exceed
// 64 bits. Only the DataAcquisition DQDATA field needs this: it packs up to
// MAX_DAQ_WORDS 64-bit elements (current PC, prev PC, data, addr, dtype/dsize,
// DirectData), so the scalar fldVal would truncate it. The mirror tracks the
// same sliding window as fldVal (OR on accumulate, shift on field extraction).
#define DAQ_WIDE_WORDS 4   // 256 bits = up to 4 x 64-bit DQDATA elements
static unsigned long long fldWide[DAQ_WIDE_WORDS];
static unsigned long long g_daqData[DAQ_WIDE_WORDS]; // captured DQDATA elements
static int                g_daqDataBits = 0;         // DQDATA bit length

static void WideReset(void) { for (int i = 0; i < DAQ_WIDE_WORDS; i++) fldWide[i] = 0; }

// OR a small (<= mdo-bits) value v into the wide accumulator at bit position pos.
static void WideOr(unsigned long long v, int pos)
{
  int word = pos >> 6, off = pos & 63;
  if (word >= DAQ_WIDE_WORDS) return; // guard: payload longer than we track
  fldWide[word] |= v << off;
  if (off > 58 && word + 1 < DAQ_WIDE_WORDS) // straddles into next word
    fldWide[word + 1] |= v >> (64 - off);
}

// Shift the wide accumulator right by n bits (n in [0,63]); mirrors fldVal >>= n.
static void WideShr(int n)
{
  if (n <= 0) return;
  for (int i = 0; i + 1 < DAQ_WIDE_WORDS; i++)
    fldWide[i] = (fldWide[i] >> n) | (fldWide[i + 1] << (64 - n));
  fldWide[DAQ_WIDE_WORDS - 1] >>= n;
}

static int NexusFieldGet(const char *name, unsigned long long *p)
{
  for (int d = msgFieldPos; nexusMsgDef[d].def != 1; d++)
  {
    if (strcmp(nexusMsgDef[d].name, name) == 0)
    {
      int fi = d - msgFieldPos;
      if (fi > msgFieldCnt) *p = 0;
      else      *p = msgFields[fi];
      return 1; // OK
    }
  }
  return 0;
}

#define NEX_FLDGET(n) unsigned long long n = 0; if (!NexusFieldGet(#n, &n)) return (-1)

// ACT-CAP command opcodes, DAQ_DTYPE_LOAD and DaqMemEvt() now live in the
// shared CttdDaq.{h,c} module (used by both N-Trace and E-Trace front-ends).

// Emit a generic export event (non control-flow): value1/value2 carry the
// event-specific payload per cttd_export.h.
static void EmitExport(CttdExportEventType type, uint64_t v1, uint64_t v2,
                       uint64_t cycle)
{
  CttdExportEvent e;
  e.source_id = (uint8_t)nexdeco_src;
  e.type = type;
  e.value1 = v1;
  e.value2 = v2;
  e.cycle_count = cycle;
  e.comment = NULL;
  cttd_export_emit(&e);
}

static int MsgHandle(FILE *f, int disp)
{
  // Multi-target mode: the context switch already happened at end-of-message
  // (NexusDeco loop); here only the output file is redirected to the active
  // target's -pcout.
  f = CttdTargetActiveOut(f);

  int TCODE = (int)msgFields[0];
  // Diagnostic walk trace: CTTD_TRACE_FROM=<msgidx> enables, from that
  // message on, one state line per message (position before the handler).
  {
    static long traceFrom = -2;
    if (traceFrom == -2) {
      const char *e = getenv("NEXRV_TRACE_FROM");
      traceFrom = e ? atol(e) : -1;
    }
    if (traceFrom >= 0 && (long)nexdeco_curMsgIdx >= traceFrom)
      fprintf(stderr, "WALK MSG#%u TCODE=%d pc=%s lastAddr=%s branchSrc=%s adj=%d\n",
              nexdeco_curMsgIdx, TCODE, AStr(nexdeco_pc), AStr(nexdeco_lastAddr),
              AStr(nexdeco_branchSrc), resourceFull_ICNT);
  }
  switch (TCODE)
  {
    case NEXUS_TCODE_DirectBranch:
      {
        NEX_FLDGET(ICNT);
        NEX_FLDGET(TSTAMP);
        tstampGlobal += TSTAMP;

        // Pre-lock foreign content: skip until the first sync (see the
        // IndirectBranchHist handler for the full rationale).
        if (nexdeco_pc & 1) break;

        // Accemic BP: a DirectBranch walk's final branch is taken by message
        // contract. Emitted by AMD's native BTM encoder and, since seq 24, by
        // CTTE in BTM mode (InstMode=3, CT_EN_BTM); decoded identically.
        nexdeco_bpFinalTaken = 1;
        int emitRet = EmitICNT(f, ICNT, 0x0, disp, tstampGlobal);
        nexdeco_bpFinalTaken = 0;
        if (emitRet < 0) return (-2);
      }
      break;

    case NEXUS_TCODE_IndirectBranch:
      {
        NEX_FLDGET(BTYPE);
        NEX_FLDGET(ICNT);
        NEX_FLDGET(UADDR);
        NEX_FLDGET(TSTAMP);
        tstampGlobal += TSTAMP;

        // Pre-lock foreign content: skip until the first sync (see the
        // IndirectBranchHist handler for the full rationale).
        if (nexdeco_pc & 1) break;

        if (EmitICNT(f, ICNT, 0x0, disp, tstampGlobal) < 0) return (-2);

        // EmitICNT walks to the branch instruction; its SOURCE PC is nexdeco_branchSrc.
        // (nexdeco_pc may have been overwritten with the call-stack return target by
        // the RET path, so it is NOT the source here.)
        Nexus_TypeAddr origin_pc = nexdeco_branchSrc;
        InfoAddr a;
        unsigned int info = InfoGet((InfoAddr)origin_pc, &a);

        // Check the reached instruction really is an indirect branch.
        // Accemic sijump: a JD/CD-with-target source (from a -sijump pcinfo)
        // may legitimately arrive as a full IndirectBranch (AMD BTM emits
        // TCODE 4 for every jalr; CTTE falls back to a full message when
        // the dynamic pair adjacency broke). Defer the verdict until the
        // UADDR below resolves the trace target: accept iff it equals the
        // static target (always true for a genuine sequential pair).
        int sijDeferred = 0;
        if (BTYPE == 0b00 /* Indirect-branch */) {
            if (info == 0)
            {
                printf("EmitErrorMsg: info == 0 @%s\n", AStr(origin_pc));
                return EmitErrorMsg("No entry in -pcinfo found.");
            }

            if ((info & INFO_INDIRECT) == 0) {
                if (info & INFO_JUMP)
                {
                    sijDeferred = 1; // verify against the resolved target below
                }
                else
                {
                // Unpadded on purpose (upstream "0x%0x"): this line is not part
                // of the width contract, so it stays byte-identical either way.
                printf("Error: Instruction 0x%" PRIx64 " not an indirect branch.\n", (uint64_t)origin_pc);
                exit(EXIT_FAILURE);
                }
            }
        }

        // Traps (BTYPE 1/2/3) are call-stack NEUTRAL: neither the trap entry nor the
        // handler's mret/sret touch the function return stack. mret returns to mepc/sepc
        // (mepc+4 for ecall, but the preempted PC for interrupts) -- the stack cannot
        // predict that, so the encoder carries the target explicitly in UADDR and mret is
        // classified as an indirect JUMP (JI, not R). Hence: no push here, no pop/check at mret.
        // (Real calls/returns INSIDE a handler still balance among themselves.)

        // Trace UADDR is authoritative for the continued decode.
        nexdeco_lastAddr ^= (UADDR << NEXUS_PARAM_AddrSkip);
        nexdeco_pc = nexdeco_lastAddr;

        // Deferred sijump verdict (see above): static target must equal the
        // resolved trace target, else this is a real desync.
        if (sijDeferred)
        {
            if (a != nexdeco_pc)
            {
                printf("Error: Instruction 0x%" PRIx64 " not an indirect branch "
                       "(static target %s != trace target %s).\n",
                       (uint64_t)origin_pc, AStr(a), AStr(nexdeco_pc));
                exit(EXIT_FAILURE);
            }
        }

        // Cross-check the mirror against the EXPLICIT return target this message
        // carries. A mismatch re-anchors (non-fatal) -- see CallStackReanchor().
        // Skip when the mirror was empty (sentinel 1).
        if (BTYPE == 0b00 && (info & INFO_RET) && conf_CallStack > 0
            && nexdeco_stackRet != 1) {
            if (nexdeco_stackRet != nexdeco_pc) {
                CallStackReanchor(origin_pc, nexdeco_stackRet, nexdeco_pc);
            }
        }

        // Classify the indirect instruction for the export event.
        CttdExportEventType evt = CTTD_EXPORT_EVT_BRANCH_TAKEN;
        if (info & INFO_CALL) evt = CTTD_EXPORT_EVT_CALL;
        else if (info & INFO_RET) evt = CTTD_EXPORT_EVT_RETURN;

        cttd_export_emit_cf(nexdeco_src, evt, origin_pc, nexdeco_pc, tstampGlobal, "indirect-branch");
      }
      break;

    case NEXUS_TCODE_ProgTraceSync:
      {
        NEX_FLDGET(SYNC); // Accemic BP: needed to spot FIFO-overrun recovery
        NEX_FLDGET(ICNT);
        NEX_FLDGET(FADDR);
        NEX_FLDGET(TSTAMP);

        // From RISC-V N-Trace Spec 8.5 Timestamp Reporting:
        // "If timestamp is enabled, all Synchronizing Messages include an absolute timestamp value with"
        tstampGlobal = TSTAMP;

        int emitRet = EmitICNT(f, ICNT, 0, disp, tstampGlobal);
        if (emitRet < 0) return (-2);
        nexdeco_lastAddr = (FADDR << NEXUS_PARAM_AddrSkip);
        nexdeco_pc = nexdeco_lastAddr;
        if (emitRet == 0) EmitSyncPC(f, nexdeco_pc);

        // Accemic BP: a FIFO-overrun recovery sync (SYNC=7) resets the
        // predictor on both sides -- neither model saw the dropped gap.
        // SYNC=7 (FIFO overrun) AND SYNC=5 (TRACE_ENABLE) both imply the
        // encoder cleared its mirror models: the overrun clears them at the
        // recovery anchor, and a trace pause clears them at the trace-off
        // correlation. The correlation is an ordinary message and can drown
        // in an overflow drop episode -- then the re-anchor sync is the ONLY
        // evidence the decoder gets (KV260 soak 2026-08-01, soak/00056:
        // encoder cleared JtcValid at the pause, decoder kept a stale entry
        // and resolved a VendorJTC onto a non-indirect instruction 400k PCs
        // later). Resetting on TRACE_ENABLE is always safe: it only ever
        // follows a pause, where the encoder has cleared as well.
        if (conf_BranchPredict && (SYNC == 7 || SYNC == 5)) BpInit();
        if (SYNC == 7 || SYNC == 5) JtcInit();
        // A TRACE_ENABLE re-anchor is a legitimate discontinuity announcement
        // (re-entry after a pause) for the per-boundary attribution.
        if (SYNC == 5) printf("MARK: SYNC-ENABLE\n");

        // SYNC
        cttd_export_emit_cf(nexdeco_src, CTTD_EXPORT_EVT_SYNC, 0, nexdeco_pc, tstampGlobal, NULL);
      }
      break;

    case NEXUS_TCODE_DirectBranchSync:
      {
        NEX_FLDGET(SYNC); // Accemic BP: needed to spot FIFO-overrun recovery
        NEX_FLDGET(ICNT);
        NEX_FLDGET(FADDR);
        NEX_FLDGET(TSTAMP);

        // From RISC-V N-Trace Spec 8.5 Timestamp Reporting:
        // "If timestamp is enabled, all Synchronizing Messages include an absolute timestamp value with"
        tstampGlobal = TSTAMP;

        // Accemic BP: the walk's final branch is taken by message contract
        // (this IS the sync-carried taken branch); the predictor is updated
        // with that outcome, mirroring the encoder's sync-path update.
        nexdeco_bpFinalTaken = 1;
        int emitRet = EmitICNT(f, ICNT, 0x0, disp, tstampGlobal);
        nexdeco_bpFinalTaken = 0;
        if (emitRet < 0) return (-2);
        nexdeco_lastAddr = (FADDR << NEXUS_PARAM_AddrSkip);
        nexdeco_pc = nexdeco_lastAddr;
        if (emitRet == 0) EmitSyncPC(f, nexdeco_pc);
        // TODO: check if address is expected a direct branch

        // Accemic BP: FIFO-overrun recovery resets the predictor (both sides).
        // (An overrun DirectBranchSync has ICNT=0 -- the branch is never
        // walked, and the encoder skips its model update to match.)
        // SYNC=7 (FIFO overrun) AND SYNC=5 (TRACE_ENABLE) both imply the
        // encoder cleared its mirror models: the overrun clears them at the
        // recovery anchor, and a trace pause clears them at the trace-off
        // correlation. The correlation is an ordinary message and can drown
        // in an overflow drop episode -- then the re-anchor sync is the ONLY
        // evidence the decoder gets (KV260 soak 2026-08-01, soak/00056:
        // encoder cleared JtcValid at the pause, decoder kept a stale entry
        // and resolved a VendorJTC onto a non-indirect instruction 400k PCs
        // later). Resetting on TRACE_ENABLE is always safe: it only ever
        // follows a pause, where the encoder has cleared as well.
        if (conf_BranchPredict && (SYNC == 7 || SYNC == 5)) BpInit();
        if (SYNC == 7 || SYNC == 5) JtcInit();
        // A TRACE_ENABLE re-anchor is a legitimate discontinuity announcement
        // (re-entry after a pause) for the per-boundary attribution.
        if (SYNC == 5) printf("MARK: SYNC-ENABLE\n");

        // SYNC
        cttd_export_emit_cf(nexdeco_src, CTTD_EXPORT_EVT_SYNC, 0, nexdeco_pc, tstampGlobal, NULL);
      }
      break;

    case NEXUS_TCODE_IndirectBranchSync:
      {
        NEX_FLDGET(SYNC); // Accemic BP: needed to spot FIFO-overrun recovery
        // NEX_FLDGET(BTYPE); // We ignore this for now
        NEX_FLDGET(ICNT);
        NEX_FLDGET(FADDR);
        NEX_FLDGET(TSTAMP);

        // From RISC-V N-Trace Spec 8.5 Timestamp Reporting:
        // "If timestamp is enabled, all Synchronizing Messages include an absolute timestamp value with"
        tstampGlobal = TSTAMP;

        int emitRet = EmitICNT(f, ICNT, 0x0, disp, tstampGlobal);
        if (emitRet < 0) return (-2);
        nexdeco_lastAddr = (FADDR << NEXUS_PARAM_AddrSkip);
        nexdeco_pc = nexdeco_lastAddr;
        if (emitRet == 0) EmitSyncPC(f, nexdeco_pc);

        // Accemic BP: FIFO-overrun recovery resets the predictor (both sides).
        // SYNC=7 (FIFO overrun) AND SYNC=5 (TRACE_ENABLE) both imply the
        // encoder cleared its mirror models: the overrun clears them at the
        // recovery anchor, and a trace pause clears them at the trace-off
        // correlation. The correlation is an ordinary message and can drown
        // in an overflow drop episode -- then the re-anchor sync is the ONLY
        // evidence the decoder gets (KV260 soak 2026-08-01, soak/00056:
        // encoder cleared JtcValid at the pause, decoder kept a stale entry
        // and resolved a VendorJTC onto a non-indirect instruction 400k PCs
        // later). Resetting on TRACE_ENABLE is always safe: it only ever
        // follows a pause, where the encoder has cleared as well.
        if (conf_BranchPredict && (SYNC == 7 || SYNC == 5)) BpInit();
        if (SYNC == 7 || SYNC == 5) JtcInit();
        // A TRACE_ENABLE re-anchor is a legitimate discontinuity announcement
        // (re-entry after a pause) for the per-boundary attribution.
        if (SYNC == 5) printf("MARK: SYNC-ENABLE\n");

        // SYNC
        cttd_export_emit_cf(nexdeco_src, CTTD_EXPORT_EVT_SYNC, 0, nexdeco_pc, tstampGlobal, NULL);
      }
      break;

    case NEXUS_TCODE_IndirectBranchHist:
      {
        NEX_FLDGET(BTYPE);
        NEX_FLDGET(ICNT);
        NEX_FLDGET(UADDR);
        NEX_FLDGET(HIST);
        NEX_FLDGET(TSTAMP);
        tstampGlobal += TSTAMP;

        // Unsynchronized (no sync message locked the PC yet): this can be
        // foreign stream content ahead of the lock point, e.g. the PREVIOUS
        // session's trace-off drain leaking into the capture head (KV260
        // robustness campaign, fixverify feat-none: leading ResourceFull +
        // IndirectBranchHist + Correlation before the first TRACE_ENABLE
        // sync). Resolving it against the sentinel PC would abort the whole
        // decode ("resolved source PC 0x00000001 ..."); skip it instead and
        // wait for the first sync.
        if (nexdeco_pc & 1) break;

        if (EmitICNT(f, ICNT, HIST, disp, tstampGlobal) < 0) return (-2);
        // Source PC is nexdeco_branchSrc (nexdeco_pc may carry the call-stack
        // return target after the RET path).
        Nexus_TypeAddr origin_pc = nexdeco_branchSrc;
        // Traps are call-stack NEUTRAL (no push); mret/sret is an indirect JUMP that reads
        // its target from UADDR, never from the stack. See the IndirectBranch handler.
        nexdeco_lastAddr ^= (UADDR << NEXUS_PARAM_AddrSkip);
        nexdeco_pc = nexdeco_lastAddr;

        // Classify (and validate) the indirect source instruction.
        InfoAddr dummy;
        unsigned int info = InfoGet((InfoAddr)origin_pc, &dummy);

        // For BTYPE==0 (genuine indirect control flow) the ICNT walk must land
        // on an indirect-branch instruction (jalr/ret/jr). Exceptions/interrupts
        // (BTYPE 1/2/3) can be taken on any instruction (including loads/stores),
        // so they are exempt. Landing on a non-indirect instruction here means
        // the decoder has desynchronized (e.g. an ICNT/HIST mismatch walked the
        // PC astray) and any further output would be garbage - abort now.
        //
        // Accemic sijump exception: with a -sijump pcinfo, a statically
        // inferable jalr is typed JD/CD (INFO_JUMP with target, NOT
        // INFO_INDIRECT) -- but the ENCODER may still emit a full IBH for it
        // when the dynamic auipc/lui adjacency was broken (e.g. an interrupt
        // between the pair halves). Accept that shape iff the static target
        // equals the trace target (for a genuine sequential pair they are
        // always equal); anything else remains a desync abort. Plain pcinfo
        // types all jalr forms as *I, so this path never relaxes there.
        if (BTYPE == 0 && (info & INFO_INDIRECT) == 0)
        {
          InfoAddr sijA;
          unsigned int sijInfo = InfoGet((InfoAddr)origin_pc, &sijA);
          if (!((sijInfo & INFO_JUMP) && (sijA == nexdeco_pc)))
          {
          printf("\nERROR: IndirectBranchHist (BTYPE=0) at MSG #%u resolved source PC %s to a "
                 "non-indirect instruction (target would be %s).\n"
                 "       Expected an indirect branch (jalr/ret/jr) at the source; this "
                 "indicates decoder desynchronization. Aborting.\n",
                 nexdeco_curMsgIdx, AStr(origin_pc), AStr(nexdeco_pc));
          exit(EXIT_FAILURE);
          }
        }

        // Cross-check the mirror against the explicit target (see IndirectBranch).
        if (BTYPE == 0 && (info & INFO_RET) && conf_CallStack > 0
            && nexdeco_stackRet != 1) {
            if (nexdeco_stackRet != nexdeco_pc) {
                CallStackReanchor(origin_pc, nexdeco_stackRet, nexdeco_pc);
            }
        }

        // destination of indirect jump
        CttdExportEventType evt = CTTD_EXPORT_EVT_BRANCH_TAKEN;
        if (info & INFO_CALL) evt = CTTD_EXPORT_EVT_CALL;
        else if (info & INFO_RET) evt = CTTD_EXPORT_EVT_RETURN;

        cttd_export_emit_cf(nexdeco_src, evt, origin_pc, nexdeco_pc, tstampGlobal, "indirect");

        // VendorJTC cache learn: every plain (BTYPE=0) IBH that carried a
        // UADDR installs its target -- the encoder does the same on every
        // emitted (cache-miss) IBH, keeping both models bit-identical.
        if (BTYPE == 0)
        {
          unsigned int jidx = JtcIndex(nexdeco_pc);
          nexdeco_jtcCache[jidx] = (Nexus_TypeAddr)nexdeco_pc;
          nexdeco_jtcValid[jidx] = 1;
        }
      }
      break;

    case NEXUS_TCODE_VendorJTC:
      {
        // Accemic vendor extension: IndirectBranchHist whose target comes
        // from the jump-target cache (JIDX) instead of a differential UADDR.
        // Identical to the IBH handler except for the target source; the
        // cache entry is read, not updated (it already holds the target).
        NEX_FLDGET(BTYPE);
        NEX_FLDGET(ICNT);
        NEX_FLDGET(JIDX);
        NEX_FLDGET(HIST);
        NEX_FLDGET(TSTAMP);
        tstampGlobal += TSTAMP;

        // Pre-lock foreign content: skip until the first sync (see the
        // IndirectBranchHist handler for the full rationale).
        if (nexdeco_pc & 1) break;

        if (EmitICNT(f, ICNT, HIST, disp, tstampGlobal) < 0) return (-2);
        Nexus_TypeAddr origin_pc = nexdeco_branchSrc;

        if (JIDX >= NEXDECO_JTC_SIZE || !nexdeco_jtcValid[JIDX])
        {
          printf("\nERROR: VendorJTC (TCODE 57) references jump-target-cache index %llu "
                 "which is %s.\n"
                 "       Encoder and decoder cache models have diverged (or the capture "
                 "was attached mid-stream). Aborting.\n",
                 JIDX, (JIDX >= NEXDECO_JTC_SIZE) ? "out of range" : "not yet installed");
          exit(EXIT_FAILURE);
        }
        nexdeco_lastAddr = nexdeco_jtcCache[JIDX];
        nexdeco_pc = nexdeco_lastAddr;

        // Classify (and validate) the indirect source instruction (same
        // rules as IndirectBranchHist).
        InfoAddr dummy;
        unsigned int info = InfoGet((InfoAddr)origin_pc, &dummy);

        if (BTYPE == 0 && (info & INFO_INDIRECT) == 0)
        {
          // Accemic sijump exception -- see the IndirectBranchHist handler.
          InfoAddr sijA;
          unsigned int sijInfo = InfoGet((InfoAddr)origin_pc, &sijA);
          if (!((sijInfo & INFO_JUMP) && (sijA == nexdeco_pc)))
          {
          printf("\nERROR: VendorJTC (BTYPE=0) at MSG #%u resolved source PC %s to a "
                 "non-indirect instruction (target would be %s).\n"
                 "       Expected an indirect branch (jalr/ret/jr) at the source; this "
                 "indicates decoder desynchronization. Aborting.\n",
                 nexdeco_curMsgIdx, AStr(origin_pc), AStr(nexdeco_pc));
          exit(EXIT_FAILURE);
          }
        }

        // Cross-check the mirror against the cached target (see IndirectBranch).
        if (BTYPE == 0 && (info & INFO_RET) && conf_CallStack > 0
            && nexdeco_stackRet != 1) {
            if (nexdeco_stackRet != nexdeco_pc) {
                CallStackReanchor(origin_pc, nexdeco_stackRet, nexdeco_pc);
            }
        }

        CttdExportEventType evt = CTTD_EXPORT_EVT_BRANCH_TAKEN;
        if (info & INFO_CALL) evt = CTTD_EXPORT_EVT_CALL;
        else if (info & INFO_RET) evt = CTTD_EXPORT_EVT_RETURN;

        cttd_export_emit_cf(nexdeco_src, evt, origin_pc, nexdeco_pc, tstampGlobal, "indirect-jtc");
      }
      break;

    case NEXUS_TCODE_VendorBP:
      {
        // Accemic vendor extension: BCNT correctly predicted direct branches
        // since the last PC-walking message, then exactly ONE branch whose
        // outcome is the inverse of the prediction. Walk them eagerly (the
        // ResourceFull RCODE-1/2 pattern): the message carries no ICNT, so
        // the walked halfwords are subtracted from the next ICNT-bearing
        // packet via the shared adjustment accumulator.
        NEX_FLDGET(BCNT);
        NEX_FLDGET(TSTAMP);
        tstampGlobal += TSTAMP;

        if (!conf_BranchPredict)
        {
          printf("\nERROR: VendorBP (TCODE 56) encountered without the -bp decode "
                 "option.\n"
                 "       This capture was recorded with InstEnBranchPrediction set; "
                 "re-run -deco with -bp. Aborting.\n");
          exit(EXIT_FAILURE);
        }

        nexdeco_bpRemain = (int)BCNT + 1;
        int doneICNT = EmitICNT(f, -1, 0, disp, tstampGlobal);
        if (doneICNT < 0) return doneICNT;

        if (nexdeco_pc & 1)
        {
          // Not yet synchronized: nothing was walked (mirrors the RCODE-1/2
          // pre-sync no-op). Drop the pending walk.
          nexdeco_bpRemain = 0;
        }
        else if (nexdeco_bpRemain != 0)
        {
          printf("\nERROR: VendorBP (TCODE 56) walk ended after %llu of %llu "
                 "branches (hit an indirect control transfer at PC %s).\n"
                 "       Encoder and decoder predictor models have diverged. "
                 "Aborting.\n",
                 BCNT + 1 - (unsigned long long)nexdeco_bpRemain, BCNT + 1,
                 AStr(nexdeco_pc));
          exit(EXIT_FAILURE);
        }
        else
        {
          resourceFull_ICNT -= doneICNT; // Consume, so next ICNT is adjusted
        }
      }
      break;

    case NEXUS_TCODE_IndirectBranchHistSync:
      {
        // NEX_FLDGET(SYNC); // We ignore this for now
        // NEX_FLDGET(BTYPE); // We ignore this for now
        // NEX_FLDGET(CANCEL); // Not used
        NEX_FLDGET(ICNT);
        NEX_FLDGET(FADDR);
        NEX_FLDGET(HIST);
        NEX_FLDGET(TSTAMP);

        // From RISC-V N-Trace Spec 8.5 Timestamp Reporting:
        // "If timestamp is enabled, all Synchronizing Messages include an absolute timestamp value with"
        tstampGlobal = TSTAMP;

        int emitRet = EmitICNT(f, ICNT, HIST, disp, tstampGlobal);
        if (emitRet < 0) return (-2);
        nexdeco_lastAddr = (FADDR << NEXUS_PARAM_AddrSkip);
        nexdeco_pc = nexdeco_lastAddr;
        if (emitRet == 0) EmitSyncPC(f, nexdeco_pc);

        // SYNC
        cttd_export_emit_cf(nexdeco_src, CTTD_EXPORT_EVT_SYNC, 0, nexdeco_pc, tstampGlobal, NULL);
      }
      break;

    // Accemic/ISTO RepeatInstruction (31) / RepeatInstructionSync (32):
    // single-instruction spin-loop compression (a taken branch targeting
    // its own address). Contract (matches ct_L2_msg_gen, seq 24 B3):
    //   31: walk ICNT with HIST (the walk ends back ON the loop PC after
    //       resolving the run's first, normally accounted iteration), then
    //       re-emit the loop PC RCNT times. The replays touch NO ICNT
    //       accounting (encoder restarts ICNT at zero after this message),
    //       so resourceFull_ICNT is not involved.
    //   32: like 31 with RCNT+1 replays (the counted iterations plus the
    //       sync-carrying one, whose halfwords the encoder dropped), then
    //       a hard re-anchor at FADDR (synchronizing message, absolute
    //       TSTAMP).
    case NEXUS_TCODE_RepeatInstruction:
    case NEXUS_TCODE_RepeatInstructionSync:
      {
        NEX_FLDGET(RCNT);
        NEX_FLDGET(ICNT);
        NEX_FLDGET(HIST);
        NEX_FLDGET(TSTAMP);

        unsigned long long repeats = RCNT;
        unsigned long long faddrFull = 0; // full anchor address (RV32 and RV64)
        if (TCODE == NEXUS_TCODE_RepeatInstructionSync)
        {
          NEX_FLDGET(FADDR);     // only the sync form carries an anchor
          faddrFull = FADDR;
          tstampGlobal = TSTAMP; // synchronizing: absolute timestamp
          repeats += 1;          // + the sync-carrying iteration itself
        }
        else
        {
          tstampGlobal += TSTAMP;
        }

        if (EmitICNT(f, (int)ICNT, HIST, disp, tstampGlobal) < 0) return (-2);

        for (unsigned long long r = 0; r < repeats; r++)
        {
          fprintf(f, "%s", AStr(nexdeco_pc));
          printf("%d PC: %s \n", nInstr, AStr(nexdeco_pc));
          nInstr++;
          if (disp & 0x10)
          {
            // The loop PC is by construction a taken direct branch.
            InfoAddr a;
            unsigned int info = InfoGet((InfoAddr)nexdeco_pc, &a);
            fprintf(f, ",B%c", (info & INFO_4) ? '4' : '2');
          }
          fprintf(f, "\n");
        }

        if (TCODE == NEXUS_TCODE_RepeatInstructionSync)
        {
          nexdeco_lastAddr = (faddrFull << NEXUS_PARAM_AddrSkip);
          nexdeco_pc = nexdeco_lastAddr;
          cttd_export_emit_cf(nexdeco_src, CTTD_EXPORT_EVT_SYNC, 0, nexdeco_pc, tstampGlobal, NULL);
        }
      }
      break;

    case NEXUS_TCODE_ResourceFull:
      {
        // The encoder does not emit TSTAMP for ResourceFull, so we leave
        // tstampGlobal alone and pass the current value through to EmitICNT.
        NEX_FLDGET(RCODE);
        if (RCODE == 1 || RCODE == 2)  // HIST_OVERFLOW (1) or HIST_OVERFLOW_REPEATED (2)
        {
          // Determine repeat count (only RCODE=2 carries HREPEAT on the wire).
          unsigned int hRepeat = 1;
          NEX_FLDGET(RDATA);
          if (RCODE == 2)
          {
            NEX_FLDGET(HREPEAT);
            hRepeat = HREPEAT;
          }
          
          if (RDATA > 1)
          {
            // Special calling to emit HIST only ...
            if (dispHistRepeat)
            {
              if (disp & 4) printf("RepeatHIST,0x%llX,%d\n", RDATA, dispHistRepeat);
              dispHistRepeat = 0;
            }
            do
            {
              // ICNT is unknown (-1): walk only the overflowed HIST bits to emit
              // the resolved PCs. The encoder does NOT reset its instruction
              // counter when a HIST-overflow ResourceFull is sent (the message
              // carries no I-CNT field, and per the spec I-CNT resets only when
              // an I-CNT field is transmitted), so the half-words we just walked
              // are still counted in the following packet's I-CNT. Subtract them
              // so the next EmitICNT adjusts that packet's ICNT down accordingly.
              int doneICNT = EmitICNT(f, -1, RDATA, disp, tstampGlobal);
              if (doneICNT < 0) return doneICNT;

              resourceFull_ICNT -= doneICNT;  // Consume, so next time ICNT will be adjusted
              hRepeat--;
            } while (hRepeat > 0);
          }
        }
        else
        if (RCODE == 0)
        {
          // This is I-CNT overflow
          NEX_FLDGET(RDATA);

          // Only accumulate once the decoder is synchronized. Before the first
          // sync locks the PC, a ResourceFull can be foreign content — e.g. the
          // PREVIOUS session's trace-off drain leaking into the capture head
          // (KV260 robustness campaign finding B2: a leading RCODE=0 with
          // RDATA=10688 survived the SYNC=5 re-anchor and made the first
          // IndirectBranchHist walk overshoot into a jalr, "indirect address
          // encountered in ICNT"). Its half-words describe instructions the
          // walk after the lock point will never visit.
          if (!(nexdeco_pc & 1))
            resourceFull_ICNT += (int)RDATA;  // Accumulate, so next time ICNT will be adjusted
          else if (disp & 1)
            printf(". pre-sync ResourceFull ICNT %d ignored (unsynchronized)\n", (int)RDATA);
        }
      }
      break;

    case NEXUS_TCODE_ProgTraceCorrelation:
      {
        NEX_FLDGET(EVCODE);
        NEX_FLDGET(CDF);
        NEX_FLDGET(ICNT);
        NEX_FLDGET(TSTAMP);
        tstampGlobal += TSTAMP;

        if (CDF == 1)
        {
          NEX_FLDGET(HIST);
          if (EmitICNT(f, ICNT, HIST, disp, tstampGlobal) < 0) return (-2);
        }
        else
        {
          // No history ...
          if (EmitICNT(f, ICNT, 0, disp, tstampGlobal) < 0) return (-2);
        }
        // Inline marker AFTER the correlation walk (judge attribution: the
        // boundary lies behind the ICNT-walk PCs; a marker BEFORE the walk
        // misses it by the walk length -- soak 00031, a one-boundary false
        // alarm on ICNT>0 correlations).
        printf("MARK: CORR EVCODE=%llu\n", EVCODE);


        // Trace off = the stream PAUSES here: instructions executed during
        // the pause are invisible, so every position-dependent decoder state
        // is stale the moment the correlation ends. Go fully UNSYNCHRONIZED
        // (same drop as the Nexus-Error handler): any content between this
        // correlation and the re-anchoring *Sync -- e.g. in-flight straggler
        // VendorBP/ResourceFull from the encoder's off-edge pipeline (KV260
        // robustness campaign rob_ovfanchor 00043: five VendorBP BCNT=0
        // between correlation and TRACE_ENABLE sync derailed the walk into
        // the 0x40 idle self-loop for 20M PCs) -- is skipped by the existing
        // pre-lock guards, and the sync's own ICNT walk (it may carry
        // post-resume pre-anchor counts) is skipped by EmitICNT's sentinel
        // check; the sync then re-locks as a pure anchor.
        nexdeco_pc        = 1;   // odd => unsynchronized until next sync
        nexdeco_lastAddr  = 1;
        nexdeco_branchSrc = 1;
        nexdeco_stackRet  = 1;
        resourceFull_ICNT = 0;   // pending ICNT adjustment dies with the pause
        CallStack_Init();        // calls/returns during the pause are invisible
                                 // (encoder empties ret_sp at the pause edge)

        // Accemic BP: trace-off resets the predictor on both sides so a
        // following session starts from the common weakly-not-taken state.
        if (conf_BranchPredict) BpInit();
        // Accemic JTC: the encoder clears JtcValid at trace-off/debug/
        // low-power (ct_L2_msg_gen correlation arm) -- mirror it, or the
        // first post-resume VendorJTC hits a stale mirror entry.
        JtcInit();
      }
      break;

    case NEXUS_TCODE_Error:
    {
      // A Nexus Error (e.g. ETYPE=QueueOverrun) means the encoder dropped trace
      // bytes: all decoder state accumulated since the last sync is now
      // unreliable. Go fully unsynchronized and drop pending context so we
      // cleanly re-lock on the following synchronization message instead of
      // decoding across the gap with stale state. (An Error is always followed
      // by a *Sync that re-installs the PC.)
      // Inline marker for the loss bookkeeping of the campaign judge
      // (verdict.py V2, per-boundary attribution): every segment boundary
      // must have its announcement LOCALLY, between the PC lines.
      NEX_FLDGET(ETYPE);
      NEX_FLDGET(ECODE);
      (void)ETYPE;
      printf("MARK: ERROR-MSG\n");

      // DATA-ONLY LOSS (ECODE names exactly the data-trace class, 0x02):
      // the instruction trace was NOT touched, and the encoder therefore
      // does NOT follow this message with a re-anchoring sync. Resetting
      // the instruction walk here would discard precisely the trace the
      // announcement promises is intact, and the walk would then stay dead
      // until the next periodic sync.
      // This is the CTTE data-trace drop policy (trTeDataControl.DataDropEna,
      // work package P7): at a queue watermark the encoder sheds data-trace
      // messages to protect the instruction trace and announces each drop
      // episode with exactly this single-class Error. Any other ECODE -- in
      // particular the generic overflow mask, which includes the
      // control-flow bit 0x04 -- keeps the full reset below.
      // Only the data-trace reference is stale: the encoder re-anchors it
      // with the next 13/14 full-address form (P3 trigger T2c), exactly as
      // after a generic Error.
      if (ECODE == 0x02) {
        nexdeco_lastDaddrValid = 0;
        break;
      }

      nexdeco_pc        = 1;   // odd => unsynchronized until next sync
      nexdeco_lastAddr  = 1;
      nexdeco_branchSrc = 1;
      nexdeco_stackRet  = 1;
      nexdeco_lastDaddrValid = 0; // DF XOR reference is stale across the gap;
                               // the encoder re-anchors with a 13/14 (T2c)
      resourceFull_ICNT = 0;   // drop pending ICNT adjustment across the gap
      CallStack_Init();        // stale call frames are meaningless after a gap
      BpInit();                // predictor state is stale across the gap too
      break;
    }

      case NEXUS_TCODE_DataWrite:
      case NEXUS_TCODE_DataRead:
      case NEXUS_TCODE_DataWriteSync:  // P3: synchronizing 5/6 forms carrying
      case NEXUS_TCODE_DataReadSync:   // the FULL address (XOR re-anchor)
      {
        // Wire: DSZ[4], ELSZ[3], DADDR(var), DATA(var), TSTAMP(var).
        // 5/6 and 13/14 share this layout; only the DADDR semantics differ:
        // 13/14 always carry the FULL address, 5/6 carry the XOR against the
        // previous data-trace message's address when compression is active
        // (trTeDataAddrCompress = XOR), the full address otherwise.
        int isSync  = (TCODE == NEXUS_TCODE_DataWriteSync || TCODE == NEXUS_TCODE_DataReadSync);
        int isWrite = (TCODE == NEXUS_TCODE_DataWrite     || TCODE == NEXUS_TCODE_DataWriteSync);
        NEX_FLDGET(DSZ);
        NEX_FLDGET(DADDR);
        NEX_FLDGET(DATA);
        NEX_FLDGET(TSTAMP);

        Nexus_TypeAddr full = (Nexus_TypeAddr)DADDR;
        if (isSync)
        {
          // Synchronizing message: absolute TSTAMP (N-Trace 8.5 rule) --
          // re-base like the CF *Sync handlers, do NOT accumulate.
          tstampGlobal = TSTAMP;
          // The full address (re-)seats the XOR reference.
          nexdeco_lastDaddr = full;
          nexdeco_lastDaddrValid = 1;
          // Stream evidence (mode source 3, see the conf_DfXor extern note):
          // the encoder emits 13/14 only with compression active, so every
          // following 5/6 carries an XOR delta. Without this, a data-only
          // stream (no config message) decoded without -dfxor would
          // misinterpret those deltas as full addresses -- silently.
          if (!conf_DfXor)
          {
            conf_DfXor = 1;
            printf("INFO: Data%sSync (TCODE %d): DF address XOR decode enabled (stream evidence)\n",
                   isWrite ? "Write" : "Read", TCODE);
          }
        }
        else
        {
          tstampGlobal += TSTAMP;
          if (conf_DfXor)
          {
            if (!nexdeco_lastDaddrValid)
            {
              // The T2 re-anchor contract guarantees a 13/14 before the
              // first XOR'd 5/6; getting here means the anchor was lost
              // (mid-stream capture attach, or post-ERROR content that
              // outran its re-anchor). The XOR delta is unreconstructable --
              // skip the event rather than exporting a garbage address.
              // (TSTAMP above was still accumulated: the message IS in the
              // stream, its delta is real.)
              printf("WARNING: XOR-compressed Data%s (MSG #%u) without a 13/14 anchor - data event skipped\n",
                     isWrite ? "Write" : "Read", nexdeco_curMsgIdx);
              break;
            }
            full = nexdeco_lastDaddr ^ (Nexus_TypeAddr)DADDR;
            nexdeco_lastDaddr = full; // reference = previous DF message's address
          }
        }

        // DSZ is the access size in BYTES (nexus_dsz_e: 1/2/4/8; 0 = the
        // Nexus "zero-data" size) -- NOT log2. DaqMemEvt takes nbytes as-is.
        CttdExportEventType evt = DaqMemEvt(
            isWrite ? 1 /* any non-LOAD => write */ : DAQ_DTYPE_LOAD,
            (unsigned)DSZ);
        // Authoritative per-event line for scripts/decode_and_check.sh
        // --data: with XOR active the raw DADDR field line in the -full log
        // is a delta, so the awk keys on THIS line instead (format equals
        // cpu_model's expected.data: KIND,0x<addr>,<bytes>). The address obeys
        // the same width contract as the PC lines (8 hex digits on a 32-bit
        // stream, 16 once the config message declared ADDR64) -- a data
        // address is as wide as the core's address space.
        if (disp & 2) printf(". DFEVT %s,%s,%u\n",
                             isWrite ? "STORE" : "LOAD",
                             AStr(full), (unsigned)DSZ);
        EmitExport(evt, full, DATA, tstampGlobal);
      }
      break;

      case NEXUS_TCODE_DataAcquisition:
      {
        // IDTAG = ACT-CAP command; DQDATA = packed 64-bit elements captured in
        // g_daqData[]. TSTAMP follows. See spec <<sec:export_native>>.
        unsigned cmd = (unsigned)msgFields[1];
        NEX_FLDGET(TSTAMP);
        tstampGlobal += TSTAMP;
        uint64_t ts = tstampGlobal;

        uint64_t el0 = g_daqData[0], el1 = g_daqData[1], el2 = g_daqData[2];

        // Shared mapping (identical for N-Trace and E-Trace vendor DAQ).
        cttd_daq_emit((uint8_t)nexdeco_src, cmd, el0, el1, el2, ts);
      }
      break;

      case NEXUS_TCODE_DeviceID:
      {
        // Device ID (TCODE 1, P4): a static identifier, PC-neutral -- no
        // walk, no ICNT booking, no address reference update. Like every
        // non-synchronizing message it carries a TSTAMP DELTA, which must
        // be accumulated (the Ownership arm used to miss exactly this and
        // drifted tstampGlobal low).
        NEX_FLDGET(DEVID);
        NEX_FLDGET(TSTAMP);
        tstampGlobal += TSTAMP;
        if (disp & 2) printf(". DEVID 0x%08llx\n", (unsigned long long)DEVID);
      }
      break;

      case NEXUS_TCODE_Watchpoint:
      {
        // Watchpoint (TCODE 15, P4): WPHIT is the bitmap of the watchpoints
        // that fired. PC-neutral as well; the payload is exported as its own
        // event so a consumer can correlate it with the program flow.
        NEX_FLDGET(WPHIT);
        NEX_FLDGET(TSTAMP);
        tstampGlobal += TSTAMP;
        if (disp & 2) printf(". WPHIT 0x%04llx\n", (unsigned long long)WPHIT);
        EmitExport(CTTD_EXPORT_EVT_WATCHPOINT, 0, WPHIT, tstampGlobal);
      }
      break;

      case NEXUS_TCODE_Ownership:
      {
        // Beifang (P4-Analyse / D-P3-9): this arm consumed no fields at all,
        // so an Ownership TSTAMP delta silently went missing from
        // tstampGlobal -- every reconstructed absolute time between the
        // Ownership message and the next sync drifted low by that delta.
        // Ownership is not synchronizing: accumulate like the other
        // non-sync handlers. (The PROCESS payload itself is still unused.)
        NEX_FLDGET(TSTAMP);
        tstampGlobal += TSTAMP;
      }
      break;

      case NEXUS_TCODE_VendorConfig:
      {
        // Accemic config message (TCODE 58, the CTTE vendor configuration
        // message -- see doc/trace-format.adoc in the TraceEncoder repository): the
        // encoder describes its own configuration in-band; the decoder
        // configures itself from it. Explicitly given CLI flags win. The
        // message is PC-neutral (no walk, no ICNT booking).
        NEX_FLDGET(CFGVER);
        NEX_FLDGET(CAPS);
        NEX_FLDGET(ENAB);
        NEX_FLDGET(P0);
        NEX_FLDGET(P1);
        NEX_FLDGET(P2);
        NEX_FLDGET(P3);
        (void)P2;
        if (CFGVER > 1 && !nexdeco_cfgReported)
        {
          printf("INFO: config message CFGVER=%llu > 1 (decoder knows v1): applying v1 fields, extra fields ignored\n", CFGVER);
        }
        // BP walk semantics (ENAB.5, steering): without an explicit -bp the
        // stream tells us itself. BpInit() puts the predictor model into the
        // encoder-mirror start state (a config message precedes the first
        // sync, so no branch has been resolved yet).
        if ((ENAB & (1ull << 5)) && !conf_BranchPredict)
        {
          if (!conf_BranchPredictCli)
          {
            conf_BranchPredict = 1;
            BpInit();
            if (!nexdeco_cfgReported) printf("INFO: config message: BP walk enabled (ENAB.5, auto)\n");
          }
        }
        else if (!(ENAB & (1ull << 5)) && conf_BranchPredict && !conf_BranchPredictCli && !nexdeco_cfgReported)
        {
          printf("INFO: config message: ENAB.5=0 but BP walk already auto-enabled earlier in this stream\n");
        }
        // DF address XOR (ENAB.21, P3): the stream announces that the
        // encoder XOR-compresses 5/6 data addresses, re-anchoring via TCODE
        // 13/14. Enable-only (same rule as BP: an ENAB.21=0 never disables
        // an earlier enable); an explicit -dfxor is simply already-on here.
        // Data-only streams carry NO config message at all -- they are
        // covered by the 13/14 stream-evidence enable in the data handler
        // and/or the -dfxor CLI flag.
        if ((ENAB & (1ull << 21)) && !conf_DfXor)
        {
          conf_DfXor = 1;
          if (!nexdeco_cfgReported) printf("INFO: config message: DF address XOR decode enabled (ENAB.21, auto)\n");
        }
        // SRC field width (P0.SrcBits, applies only when P1.InhibitSrc=0).
        // Bootstrap rule (SPEC section 5): with InhibitSrc=1 no message
        // carries SRC fields and P0 is display-only; with InhibitSrc=0 the
        // config message ITSELF carried a SRC field, so decoding only works
        // when -src was given -- then P0 merely verifies the CLI value.
        {
          int inhibitSrc = (int)((P1 >> 11) & 1);
          int srcBits    = (int)(P0 & 0xF);
          if (!inhibitSrc)
          {
            if (cttd_conf_src_bits_cli && cttd_conf_src_bits != srcBits)
            {
              printf("WARNING: config message P0.SrcBits=%d != -src %d (CLI wins)\n", srcBits, cttd_conf_src_bits);
            }
            else if (!cttd_conf_src_bits_cli && cttd_conf_src_bits != srcBits)
            {
              // Reachable only if the config message itself was decodable
              // without SRC bits (srcBits changed mid-stream is a contract
              // violation) -- apply for subsequent messages and say so.
              cttd_conf_src_bits = srcBits;
              printf("INFO: config message: SRC width %d applied (auto)\n", srcBits);
            }
          }
        }
        if (!nexdeco_cfgReported && (CAPS & (1ull << 6)))
        {
          printf("INFO: config message: core uses sijump convention (CAPS.6) -- PCInfo must be generated with -conv ... -sijump\n");
        }
        // W4: implicit-return compression (bit 0) is the ONE feature whose
        // decode depends on the return-address mirror. The encoder folds a
        // return only when ITS stack predicted the target, so a mirror that is
        // off or SHALLOWER than the hardware stack reconstructs a wrong PC
        // without any other symptom. PARAM3[12:8] carries that hardware depth,
        // so the mismatch is detectable from the stream alone. Warn, do not
        // resize: a config message may arrive mid-stream (every trace-on edge),
        // and re-initialising the mirror there would throw away live entries.
        if (!nexdeco_cfgReported && (ENAB & 1ull))
        {
          int retDepth = (int)((P3 >> 8) & 0x1F);
          if (conf_CallStack <= 0)
          {
            printf("WARNING: stream announces implicit-return compression (ENAB.0) but the "
                   "return-address mirror is OFF (-cs 0) -- folded returns cannot be reconstructed\n");
          }
          else if (retDepth > 0 && conf_CallStack < retDepth)
          {
            printf("WARNING: return-address mirror is %d deep, the encoder announces %d "
                   "(PARAM3[12:8]) -- implicit returns from a deeper nesting decode to a WRONG "
                   "PC; use -cs %d\n", conf_CallStack, retDepth, retDepth);
          }
        }
        // Address width (CAPS.23 = ADDR64, R1.2 / X2c). The decoder state is
        // 64-bit regardless; this only widens the printed addresses to the
        // core's address width so the text diffs against cpu_model's
        // expected.pcs/expected.data line up. Enable-only like BP and DF-XOR:
        // a later CAPS.23=0 never narrows a stream that already announced 64
        // bit (a mid-stream width change is a contract violation, and the
        // already-emitted lines cannot be un-widened). There is no CLI
        // counterpart, so the announcement is the single source of truth.
        // The flag belongs to THIS source (R1.2b): it is swapped with the
        // per-target context, so a mixed-XLEN funnel keeps one width per
        // pcout instead of the widest one winning for everybody.
        // Every change is announced, not just the one carried by a source's
        // FIRST config message (audit finding C-3): a CAPS.23 arriving later
        // -- resync, attach mid-stream -- used to widen the text silently in
        // the middle of a pcout, and a line-wise --pc diff then blames the
        // decode. The late case additionally warns: the lines already written
        // are narrow and cannot be widened after the fact.
        if ((CAPS & (1ull << 23)) && !cttd_conf_addr64)
        {
          cttd_conf_addr64 = 1;
          printf("INFO: config message: 64-bit addresses (CAPS.23, auto)\n");
          if (nexdeco_cfgReported)
          {
            printf("WARNING: address width switched to 64 bit MID-STREAM (SRC=%u, MSG #%u)"
                   " -- addresses printed before this line are 8 digits wide\n",
                   nexdeco_src, nexdeco_curMsgIdx);
          }
        }
        nexdeco_cfgReported = 1;
      }
      break;

      case NEXUS_TCODE_RepeatBranch:  // Handled differently!

      default:
      return -(100 + TCODE);  // Not handled TCODE
  }

  // Multi-target mode: remember this source's last handled message for a
  // possible RepeatBranch replay (per-source, unlike the global msgFields
  // trick of single-target mode). Idempotent during the replay itself.
  if (cttd_target_mode)
  {
    for (int i = 0; i < CTTD_MSGFIELDS_MAX; i++) nexdeco_lastMsg[i] = msgFields[i];
    nexdeco_lastMsgPos = msgFieldPos;
    nexdeco_lastMsgCnt = msgFieldCnt;
    nexdeco_lastMsgValid = 1;
  }

  return 0;
}

// This function is an extension of 'NexusDump'
// It adds all fields (for each message) into fldArray and at end of each message
// it calls 'MsgHandle()' function.
int NexusDeco(FILE *f, int disp)
{
  int fldDef = -1;
  int fldBits = 0;
  Nexus_TypeField fldVal = 0;

  int msgCnt = 0;
  int msgBytes = 0;
  int msgErrors = 0;
  int msgReserved = 0;  // Reserved/unknown-TCODE messages skipped (N-Trace contract)
  int srcPending = 0;
  int skipMessage = 0;
  int skipReserved = 0; // While set: discard bytes until MSEO=='11' (EndOfMessage)

  // Make sure decoder is using real call-stack ...
  if (conf_CallStack < 0)
  {
    conf_CallStack = -conf_CallStack;
  }
  CallStack_Init();
  BpInit(); // Accemic BP: predictor starts weakly-not-taken (encoder mirror)

  msgFieldCnt = 0;  // No fields

  unsigned char msgByte = 0;
  unsigned char prevByte = 0;
  for (;;)
  {
    prevByte = msgByte;
    if (fread(&msgByte, 1, 1, fNex) != 1) break;  // EOF

/*
 
. 0x70 011100_00: TCODE[6]=28 (MSG #41) - IndirectBranchHist
. 0x10 000100_00: BTYPE[2]=0x0
. 0x49 010010_01: ICNT[10]=0x121
. 0x70 011100_00:
. 0x5D 010111_01: UADDR[12]=0x5DC
. 0xF4 111101_00:
. 0xFC 111111_00:
. 0xFC 111111_00:
. 0xFF 111111_11: HIST[24]=0xFFFFFD

 */     

#if 0 // Some debug code (it make one of tests fail!)
      if (1 && msgCnt == 42 && prevByte == 0xFC && msgByte == 0xFF)
      {
        msgByte = 0x6b;
      }
#endif      

#if 1 // This will skip long sequnece of idles (visible in true captures ...)
    if (msgByte == 0xFF && prevByte == 0xFF)
    {
      continue;
    }
#endif


    if (disp & 1)
    {
      if (msgCnt > 0 && fldDef < 0)
      {
        printf(". \n");
      }
      printf(". 0x%02X ", msgByte);
      for (int b = 0x80; b != 0; b >>= 1)
      {
        if (b == 0x2) printf("_");
        if (msgByte & b) printf("1"); else printf("0");
      }
      printf(":");
    }

    unsigned int mdo = msgByte >> 2;
    unsigned int mseo = msgByte & 0x3;

    if (mseo == 0x2)
    {
      printf(" ERROR: MSEO='10' is not allowed\n");
      return -1;  // Error return
    }

    if (skipReserved)
    {
      // Consuming the remainder of a reserved/unknown-TCODE message (see
      // below): the MDO/MSEO framing is self-delimiting, so discard bytes
      // until EndOfMessage (MSEO=='11') WITHOUT touching any of the
      // flow-reconstruction state (fldDef stays -1, no MsgHandle call).
      msgBytes++;
      if (disp & 1) printf(" (reserved message - skipped)\n");
      if (mseo == 0x3) skipReserved = 0; // EndOfMessage reached
      continue;
    }

    if (fldDef < 0)
    {
      if (mseo == 0x3)
      {
        if (disp & 1) printf(" IDLE\n");
        continue;
      }

      if (mseo != 0x0)
      {
        printf(" ERROR: Message must start from MSEO='00'\n");
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
        // RISC-V N-Trace 1.0 (ratified): TCODEs the spec does not adopt are
        // "Reserved for future extensions", and reserved messages "should be
        // ignored by decoders interested in program flow only" (they may be
        // seen when trace capture is corrupted). The MDO/MSEO framing is
        // self-delimiting, so skip the whole message (all bytes up to
        // MSEO=='11') instead of aborting - the PC reconstruction must stay
        // identical to the stream without this message.
        printf(" WARNING: Reserved/unknown TCODE=%u (MSG #%d) - message skipped\n",
               tcode, msgCnt);
        msgReserved++;
        msgCnt++;
        msgBytes++;
        skipReserved = 1;
        continue;
      }

      // Special handling for RepeatBranch message.
      // We want to preserve previous packet, so we can
      // repeat it at end of RepeatBranch handling.
      if (tcode == NEXUS_TCODE_RepeatBranch)
      {
        // Save previous message fields
        msgFields[6] = msgFieldPos;
        msgFields[7] = msgFieldCnt;
        msgFields[8] = msgFields[0];
        msgFields[9] = msgFields[1];
      }

      nexdeco_src = 0; // Default when SRC not enabled

      // Save to allow later decoding
      msgFieldPos = fldDef;
      msgFieldCnt = 0;
      msgFields[msgFieldCnt++] = tcode;

      msgBytes++;

      fldDef++;
      fldBits = 6 - NEXUS_FLDSIZE_TCODE;
      fldVal = mdo >> NEXUS_FLDSIZE_TCODE;
      WideReset();
      fldWide[0] = fldVal;
      srcPending = (cttd_conf_src_bits > 0);
      skipMessage = 0;

      if (disp & 3)
      {
        printf(" TCODE[6]=%d (MSG #%d) - %s", tcode, msgCnt, nexusMsgDef[fldDef - 1].name);
        if (!(disp & 1) || fldBits == 0) printf("\n");
      }
      msgCnt++;
      nexdeco_curMsgIdx = (unsigned int)msgCnt;

      if (tcode == NEXUS_TCODE_Error) msgErrors++;
    }
    else
    {
      // Accumulate 'mdo' to field value
      fldVal |= ((unsigned long long)mdo << fldBits);
      WideOr((unsigned long long)mdo, fldBits);
      fldBits += 6;
      msgBytes++;
    }

    // Extract SRC field (between TCODE and message-specific fields)
    if (srcPending && fldBits >= cttd_conf_src_bits)
    {
      nexdeco_src = (unsigned int)(fldVal & NexusFieldMask(cttd_conf_src_bits));
      fldVal >>= cttd_conf_src_bits;
      WideShr(cttd_conf_src_bits);
      fldBits -= cttd_conf_src_bits;
      srcPending = 0;

      if (disp & 1) printf(" SRC[%d]=%u", cttd_conf_src_bits, nexdeco_src);

      if (cttd_conf_src_filter >= 0 && (int)nexdeco_src != cttd_conf_src_filter)
      {
        skipMessage = 1;
      }
    }

    // Process fixed size fields (there may be more than one in one MDO record)
    while ((nexusMsgDef[fldDef].def & 0x200) && fldBits >= (nexusMsgDef[fldDef].def & 0xFF))
    {
      int fldSize = nexusMsgDef[fldDef].def & 0xFF;
      unsigned long long fldOut = fldVal & NexusFieldMask(fldSize);

      msgFields[msgFieldCnt++] = fldOut; // Save field

      if (disp & 1) printf(" %s[%d]=0x%llx", nexusMsgDef[fldDef].name, fldSize, fldOut);
      fldDef++;
      fldVal >>= fldSize;
      WideShr(fldSize);
      fldBits -= fldSize;
    }

    if (mseo == 0x0)
    {
      if (disp & 1) printf("\n");
      continue;
    }

    if (nexusMsgDef[fldDef].def & 0x400)
    {
      // ResourceFull's HREPEAT (the RDATA[1] packet) is present on the wire only
      // for RCODE==2 (HIST_OVERFLOW_REPEATED). For RCODE 0/1 it is absent, so
      // skip the HREPEAT def slot - otherwise the trailing TSTAMP packet would be
      // mislabeled/stored as HREPEAT. A zero placeholder keeps the msgFields[]
      // index aligned with the def position that NexusFieldGet() resolves by name.
      if (msgFields[0] == NEXUS_TCODE_ResourceFull && msgFields[1] != 2 &&
          strcmp(nexusMsgDef[fldDef].name, "HREPEAT") == 0)
      {
        msgFields[msgFieldCnt++] = 0; // absent HREPEAT (RCODE != 2)
        fldDef++;
      }

      // Variable size field
      // PRIx64/PRIu64, not %llx/%llu: Nexus_TypeField is uint64_t, which is
      // `unsigned long` on LP64 (Linux) and `unsigned long long` on LLP64
      // (Windows). A fixed length modifier is right on only one of the two
      // platforms -- this spot was the remainder of the PRIX64 cure from R1.0.
      if (disp & 1) printf(" %s[%d]=0x%" PRIx64 " (%" PRIu64 ")\n", nexusMsgDef[fldDef].name, fldBits, (uint64_t)fldVal, (uint64_t)fldVal);

      // Capture the (possibly > 64-bit) DQDATA payload from the wide mirror so
      // the DataAcquisition handler can split it into 64-bit elements.
      if (msgFields[0] == NEXUS_TCODE_DataAcquisition &&
          strcmp(nexusMsgDef[fldDef].name, "DQDATA") == 0)
      {
        for (int i = 0; i < DAQ_WIDE_WORDS; i++) g_daqData[i] = fldWide[i];
        g_daqDataBits = fldBits;
      }

      msgFields[msgFieldCnt++] = fldVal; // Save field

      if (mseo == 3)
      {
        int cnt = 1;

        dispHistRepeat = 0;

        // Multi-target mode: switch to the message's source context BEFORE
        // any per-source state (RepeatBranch replay context, decode state)
        // is touched. Unknown sources are skipped (warn-once).
        if (!skipMessage && cttd_target_mode)
        {
          if (CttdTargetSwitch(nexdeco_src) < 0)
          {
            skipMessage = 1;
          }
        }

        if (msgFields[0] == NEXUS_TCODE_RepeatBranch && !skipMessage)
        {
          // Special handling for repeat branch (which only has 1 field!)
          cnt = msgFields[1]; // Counter set in RepeatBranch message
          if (cttd_target_mode)
          {
            // Replay THIS source's last message (the globally previous
            // message may belong to a different source in a merged stream).
            if (!nexdeco_lastMsgValid)
            {
              printf("WARNING: RepeatBranch without a prior message for SRC=%u -- ignored\n", nexdeco_src);
              skipMessage = 1;
            }
            else
            {
              for (int i = 0; i < CTTD_MSGFIELDS_MAX; i++) msgFields[i] = nexdeco_lastMsg[i];
              msgFieldPos = nexdeco_lastMsgPos;
              msgFieldCnt = nexdeco_lastMsgCnt;
            }
          }
          else if (msgFields[8] == 0)
          {
            // Capture head: a RepeatBranch with NO prior message in this
            // stream (ring capture starts mid-stream; the pre-lock guards
            // inside the handlers never see this message because the replay
            // machinery sits OUTSIDE MsgHandle). The zero-initialized saved
            // TCODE would be replayed as TCODE 0 -> "-(100+0)" abort
            // (KV260 campaign #7, soak/00052: Error #100 at 0 PCs).
            // Mirror the target-mode guard above: warn once, skip.
            printf("WARNING: RepeatBranch without a prior message (capture head) -- ignored\n");
            skipMessage = 1;
          }
          else
          {
            msgFieldPos = msgFields[6]; // Restrore previous message (saved)
            msgFieldCnt = msgFields[7];
            msgFields[0] = msgFields[8];
            msgFields[1] = msgFields[9];
          }
          dispHistRepeat = cnt;
        }

        while (cnt > 0) // Handle (1 or many times ...)
        {
          if (!skipMessage)
          {
            int err = MsgHandle(f, disp);
            if (err < 0) return err;
          }
          cnt--;
        }

        skipMessage = 0;
        fldDef = -1;
      }
      else
      {
        fldDef++;
      }
      fldBits = 0;
      fldVal = 0;
      WideReset();
      continue;
    }

    if (fldBits > 0)
    {
      printf(" ERROR: Not enough bits for non-variable field\n");
      return -4;
    }
  }

  int retInstr = nInstr;
  if (cttd_target_mode)
  {
    // Save the active target's state back to its slot, then report and
    // return the per-target totals.
    CttdTargetFlushActive();
    retInstr = 0;
    for (int t = 0; t < CTTD_TARGET_MAX; t++)
    {
      if (!cttd_targets[t].used) continue;
      retInstr += cttd_targets[t].deco.nInstr;
    }
  }

  if (disp & 4)
  {
    printf("Stat: %d bytes, %d messages, %d error messages", msgBytes, msgCnt, msgErrors);
    if (msgReserved > 0) printf(", %d reserved skipped", msgReserved);
    if (msgCnt > 0) printf(", %.2lf bytes/message", ((double)msgBytes) / msgCnt);
    if (retInstr > 0) printf(", %d instr, %.3lf bits/instr", retInstr, ((double)msgBytes * 8) / retInstr);
    printf("\n");
    // Printed only when it happened, so a stream without context switches keeps
    // its historic stdout byte for byte (W4).
    if (nexdeco_csReanchor > 0)
      printf("Stat: %u call-stack re-anchors (explicit return target != mirror prediction)\n",
             nexdeco_csReanchor);
    if (cttd_target_mode)
    {
      for (int t = 0; t < CTTD_TARGET_MAX; t++)
      {
        if (!cttd_targets[t].used) continue;
        printf("Stat: target %d: %d instr\n", t, cttd_targets[t].deco.nInstr);
      }
    }
  }

  return retInstr; // Number of instructions generated (all targets)
}

//****************************************************************************
// End of cttd_deco.c file
