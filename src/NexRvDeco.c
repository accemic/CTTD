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
// File NexRvDeco.c  - Nexus RISC-V Trace decoder reference implementation

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
#include "NexRvInfo.h" //  Definition of Nexus messages
#include "NexRvExport.h"

// Decoder works on two files and dumper on first file
extern FILE *fNex;      // Nexus messages (binary bytes)

#if 1 // Callstack related
extern int conf_CallStack;
extern void CallStack_Init();
extern void CallStack_Push(Nexus_TypeAddr ret);
extern Nexus_TypeAddr CallStack_Pop();
#endif

static unsigned int nexdeco_pc        = 1; // odd => unsynchronized until first sync message installs a PC
static unsigned int nexdeco_lastAddr  = 1;
static unsigned int nexdeco_src       = 0; // Current message SRC field value
static int nInstr = 0;

// Additive instrumentation over upstream EmitICNT (control flow kept identical):
//  - nexdeco_branchSrc: PC of the last instruction the ICNT walk visited, i.e. the
//    branch SOURCE. The indirect-branch handlers read this instead of nexdeco_pc,
//    which the call-stack RET path transiently overwrites with the return TARGET.
//  - nexdeco_stackRet: last address popped from the call stack during the walk
//    (NexRv.c CallStack_Pop() returns 1 as the empty-stack sentinel). Used to
//    cross-check the explicit (uncompressed) return target carried by UADDR.
static unsigned int nexdeco_branchSrc = 1;
static Nexus_TypeAddr nexdeco_stackRet = 1;

// Repeat count carried from a RepeatBranch message into the message it repeats,
// so the ResourceFull (RCODE=2) handler can emit a "RepeatHIST" verbose line
// (matches upstream 6cd138f).
static int dispHistRepeat = 0;

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

static int EmitErrorMsg(const char *err)
{
  printf("\nERROR: %s\n", err);
  return -10;
}

static void EmitSyncPC(FILE *f, unsigned int pc)
{
  // Do not write sync address to pcout — it will be emitted by the
  // subsequent message's EmitICNT when it walks over this address.
  (void)f;
  printf("SYNC PC: 0x%08x\n", pc);
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
  if (disp & 1) printf(". next_iaddr=0x%08x, EmitICNT(n=%d,hist=0x%llx)\n", nexdeco_pc, n, hist);

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
    histMask = 0x1;
    while (histMask <= hist) histMask <<= 1;
    histMask >>= 2;
  }

  while (n != 0)
  {
    unsigned int origin_pc = nexdeco_pc;
    nexdeco_branchSrc = nexdeco_pc; // Track the source PC; after the loop this
                                    // holds the terminal (branch) instruction.

    fprintf(f, "0x%08x", nexdeco_pc);
    printf("%d PC: 0x%08x \n", nInstr, nexdeco_pc);
    nInstr++; // Statistics (for compression display)

    InfoAddr a;
    unsigned int info = InfoGet((InfoAddr)nexdeco_pc, &a);
    if (info == 0)
    {
      printf("EmitErrorMsg: info == 0 @0x%08x\n", nexdeco_pc);
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
      if (hist == 0)
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
      NexRvExportEventType evt = NEXRV_EXPORT_EVT_BRANCH_TAKEN;
      const char *cmt = NULL;

      if (info & INFO_CALL) {
        evt = NEXRV_EXPORT_EVT_CALL;
      } else if (info & INFO_RET) {
        evt = NEXRV_EXPORT_EVT_RETURN;
      } else if (info & INFO_NOJUMP) {
        evt = NEXRV_EXPORT_EVT_BRANCH_NOTTAKEN;
        cmt = "no-jump";
      } else {
        evt = NEXRV_EXPORT_EVT_BRANCH_TAKEN;
        cmt = "jump";
      }

      nexrv_export_emit_cf(nexdeco_src, evt, origin_pc, nexdeco_pc, tstamp, cmt);
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
// nbytes is the access size in BYTES, matching the encoder's Nexus DSZ field
// (NEXUS_DSZ_1/2/4/8 = 1/2/4/8) and the CTXP MEMx_<n> suffix directly. Callers
// that hold a log2 size must convert (1u << log2) before calling.
static NexRvExportEventType DaqMemEvt(unsigned dtype, unsigned nbytes)
{
  int isWrite = (dtype != DAQ_DTYPE_LOAD);
  switch (nbytes)
  {
    case 1:  return isWrite ? NEXRV_EXPORT_EVT_MEMWRITE_1 : NEXRV_EXPORT_EVT_MEMREAD_1;
    case 2:  return isWrite ? NEXRV_EXPORT_EVT_MEMWRITE_2 : NEXRV_EXPORT_EVT_MEMREAD_2;
    case 4:  return isWrite ? NEXRV_EXPORT_EVT_MEMWRITE_4 : NEXRV_EXPORT_EVT_MEMREAD_4;
    case 8:  return isWrite ? NEXRV_EXPORT_EVT_MEMWRITE_8 : NEXRV_EXPORT_EVT_MEMREAD_8;
    default: return isWrite ? NEXRV_EXPORT_EVT_MEMWRITE_0 : NEXRV_EXPORT_EVT_MEMREAD_0;
  }
}

// Emit a generic export event (non control-flow): value1/value2 carry the
// event-specific payload per NexRvExport.h.
static void EmitExport(NexRvExportEventType type, uint64_t v1, uint64_t v2,
                       uint64_t cycle)
{
  NexRvExportEvent e;
  e.source_id = (uint8_t)nexdeco_src;
  e.type = type;
  e.value1 = v1;
  e.value2 = v2;
  e.cycle_count = cycle;
  e.comment = NULL;
  nexrv_export_emit(&e);
}

static int MsgHandle(FILE *f, int disp)
{
  int TCODE = (int)msgFields[0];
  switch (TCODE)
  {
    case NEXUS_TCODE_DirectBranch:
      {
        NEX_FLDGET(ICNT);
        NEX_FLDGET(TSTAMP);
        tstampGlobal += TSTAMP;

        if (EmitICNT(f, ICNT, 0x0, disp, tstampGlobal) < 0) return (-2);
      }
      break;

    case NEXUS_TCODE_IndirectBranch:
      {
        NEX_FLDGET(BTYPE);
        NEX_FLDGET(ICNT);
        NEX_FLDGET(UADDR);
        NEX_FLDGET(TSTAMP);
        tstampGlobal += TSTAMP;

        if (EmitICNT(f, ICNT, 0x0, disp, tstampGlobal) < 0) return (-2);

        // EmitICNT walks to the branch instruction; its SOURCE PC is nexdeco_branchSrc.
        // (nexdeco_pc may have been overwritten with the call-stack return target by
        // the RET path, so it is NOT the source here.)
        unsigned int origin_pc = nexdeco_branchSrc;
        InfoAddr a;
        unsigned int info = InfoGet((InfoAddr)origin_pc, &a);

        // Check the reached instruction really is an indirect branch.
        if (BTYPE == 0b00 /* Indirect-branch */) {
            if (info == 0)
            {
                printf("EmitErrorMsg: info == 0 @0x%08x\n", origin_pc);
                return EmitErrorMsg("No entry in -pcinfo found.");
            }

            if ((info & INFO_INDIRECT) == 0) {
                printf("Error: Instruction 0x%0x not an indirect branch.\n", origin_pc);
                exit(EXIT_FAILURE);
            }
        }

        // On an exception/interrupt entry (BTYPE 1/2/3) the trapped PC is the
        // address the handler's mret/sret will return to (mepc/sepc). After the
        // ICNT walk, nexdeco_pc holds exactly that PC (the next-to-execute
        // instruction), BEFORE UADDR overwrites it with the handler entry below.
        // Capture it so we can push a matching return-stack frame: the trap is
        // not a CALL instruction, so EmitICNT's CALL path never pushed it, yet
        // the later mret is INFO_RET and pops the stack. Without this push the
        // mret would pop a stale frame and the return-target check would abort.
        Nexus_TypeAddr trapRetAddr = nexdeco_pc;

        // Trace UADDR is authoritative for the continued decode.
        nexdeco_lastAddr ^= (UADDR << NEXUS_PARAM_AddrSkip);
        nexdeco_pc = nexdeco_lastAddr;

        if (BTYPE != 0b00 && conf_CallStack > 0) {
            CallStack_Push(trapRetAddr);
        }

        // Verify the call-stack prediction for returns. The trace has no return-stack
        // compression, so every return carries an explicit UADDR; the popped address
        // must match it. Skip when the stack was empty (sentinel 1).
        if (BTYPE == 0b00 && (info & INFO_RET) && conf_CallStack > 0
            && nexdeco_stackRet != 1) {
            if (nexdeco_stackRet != nexdeco_pc) {
                printf("Error: return at 0x%08x: call-stack target 0x%08x != trace target 0x%08x.\n",
                       origin_pc, (unsigned int)nexdeco_stackRet, nexdeco_pc);
                exit(EXIT_FAILURE);
            }
        }

        // Classify the indirect instruction for the export event.
        NexRvExportEventType evt = NEXRV_EXPORT_EVT_BRANCH_TAKEN;
        if (info & INFO_CALL) evt = NEXRV_EXPORT_EVT_CALL;
        else if (info & INFO_RET) evt = NEXRV_EXPORT_EVT_RETURN;

        nexrv_export_emit_cf(nexdeco_src, evt, origin_pc, nexdeco_pc, tstampGlobal, "indirect-branch");
      }
      break;

    case NEXUS_TCODE_ProgTraceSync:
      {
        // NEX_FLDGET(SYNC); // We ignore this for now
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

        // SYNC
        nexrv_export_emit_cf(nexdeco_src, NEXRV_EXPORT_EVT_SYNC, 0, nexdeco_pc, tstampGlobal, NULL);
      }
      break;

    case NEXUS_TCODE_DirectBranchSync:
      {
        // NEX_FLDGET(SYNC); // We ignore this for now
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
        // TODO: check if address is expected a direct branch

        // SYNC
        nexrv_export_emit_cf(nexdeco_src, NEXRV_EXPORT_EVT_SYNC, 0, nexdeco_pc, tstampGlobal, NULL);
      }
      break;

    case NEXUS_TCODE_IndirectBranchSync:
      {
        // NEX_FLDGET(SYNC); // We ignore this for now
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

        // SYNC
        nexrv_export_emit_cf(nexdeco_src, NEXRV_EXPORT_EVT_SYNC, 0, nexdeco_pc, tstampGlobal, NULL);
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

        if (EmitICNT(f, ICNT, HIST, disp, tstampGlobal) < 0) return (-2);
        // Source PC is nexdeco_branchSrc (nexdeco_pc may carry the call-stack
        // return target after the RET path).
        unsigned int origin_pc = nexdeco_branchSrc;
        // Trap entry (BTYPE 1/2/3): nexdeco_pc currently holds the trapped PC
        // (mepc/sepc) the handler will return to, before UADDR overwrites it.
        // See the IndirectBranch handler for the rationale.
        Nexus_TypeAddr trapRetAddr = nexdeco_pc;
        nexdeco_lastAddr ^= (UADDR << NEXUS_PARAM_AddrSkip);
        nexdeco_pc = nexdeco_lastAddr;

        if (BTYPE != 0 && conf_CallStack > 0) {
            CallStack_Push(trapRetAddr);
        }

        // Classify (and validate) the indirect source instruction.
        InfoAddr dummy;
        unsigned int info = InfoGet((InfoAddr)origin_pc, &dummy);

        // For BTYPE==0 (genuine indirect control flow) the ICNT walk must land
        // on an indirect-branch instruction (jalr/ret/jr). Exceptions/interrupts
        // (BTYPE 1/2/3) can be taken on any instruction (including loads/stores),
        // so they are exempt. Landing on a non-indirect instruction here means
        // the decoder has desynchronized (e.g. an ICNT/HIST mismatch walked the
        // PC astray) and any further output would be garbage - abort now.
        if (BTYPE == 0 && (info & INFO_INDIRECT) == 0)
        {
          printf("\nERROR: IndirectBranchHist (BTYPE=0) resolved source PC 0x%08x to a "
                 "non-indirect instruction (target would be 0x%08x).\n"
                 "       Expected an indirect branch (jalr/ret/jr) at the source; this "
                 "indicates decoder desynchronization. Aborting.\n",
                 origin_pc, nexdeco_pc);
          exit(EXIT_FAILURE);
        }

        // Verify the call-stack prediction for returns (see IndirectBranch).
        if (BTYPE == 0 && (info & INFO_RET) && conf_CallStack > 0
            && nexdeco_stackRet != 1) {
            if (nexdeco_stackRet != nexdeco_pc) {
                printf("Error: return at 0x%08x: call-stack target 0x%08x != trace target 0x%08x.\n",
                       origin_pc, (unsigned int)nexdeco_stackRet, nexdeco_pc);
                exit(EXIT_FAILURE);
            }
        }

        // destination of indirect jump
        NexRvExportEventType evt = NEXRV_EXPORT_EVT_BRANCH_TAKEN;
        if (info & INFO_CALL) evt = NEXRV_EXPORT_EVT_CALL;
        else if (info & INFO_RET) evt = NEXRV_EXPORT_EVT_RETURN;

        nexrv_export_emit_cf(nexdeco_src, evt, origin_pc, nexdeco_pc, tstampGlobal, "indirect");
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
        nexrv_export_emit_cf(nexdeco_src, NEXRV_EXPORT_EVT_SYNC, 0, nexdeco_pc, tstampGlobal, NULL);
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
              if (disp & 4) printf("RepeatHIST,0x%lX,%d\n", RDATA, dispHistRepeat);            
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

          resourceFull_ICNT += (int)RDATA;  // Accumulate, so next time ICNT will be adjusted
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
        nexdeco_pc = nexdeco_lastAddr;
      }
      break;

    case NEXUS_TCODE_Error:
      // A Nexus Error (e.g. ETYPE=QueueOverrun) means the encoder dropped trace
      // bytes: all decoder state accumulated since the last sync is now
      // unreliable. Go fully unsynchronized and drop pending context so we
      // cleanly re-lock on the following synchronization message instead of
      // decoding across the gap with stale state. (An Error is always followed
      // by a *Sync that re-installs the PC.)
      nexdeco_pc        = 1;   // odd => unsynchronized until next sync
      nexdeco_lastAddr  = 1;
      nexdeco_branchSrc = 1;
      nexdeco_stackRet  = 1;
      resourceFull_ICNT = 0;   // drop pending ICNT adjustment across the gap
      CallStack_Init();        // stale call frames are meaningless after a gap
      break;

      case NEXUS_TCODE_DataWrite:
      case NEXUS_TCODE_DataRead:
      {
        // Wire: DSZ[4], ELSZ[3], DADDR(var), DATA(var), TSTAMP(var).
        NEX_FLDGET(DSZ);
        NEX_FLDGET(DADDR);
        NEX_FLDGET(DATA);
        NEX_FLDGET(TSTAMP);
        tstampGlobal += TSTAMP;

        // DSZ is the access size in log2(bytes): 0->1, 1->2, 2->4, 3->8.
        NexRvExportEventType evt = DaqMemEvt(
            TCODE == NEXUS_TCODE_DataWrite ? 1 /* any non-LOAD => write */ : DAQ_DTYPE_LOAD,
            (unsigned)DSZ);
        EmitExport(evt, DADDR, DATA, tstampGlobal);
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

        switch (cmd)
        {
          case ACT_CAP_PC_CURR: { // (DAQ_DATA tag ->) SYNC(target=PC)
            uint64_t pc = el0, tag = el1;
            if (tag != 0) EmitExport(NEXRV_EXPORT_EVT_DAQ_DATA, 0, tag, ts);
            EmitExport(NEXRV_EXPORT_EVT_SYNC, 0, pc, ts);
          } break;

          case ACT_CAP_PC_CURR_LAST: { // (tag ->) DAQ_LAST_PC -> SYNC
            uint64_t pc = el0, lastpc = el1, tag = el2;
            if (tag != 0) EmitExport(NEXRV_EXPORT_EVT_DAQ_DATA, 0, tag, ts);
            EmitExport(NEXRV_EXPORT_EVT_DAQ_LAST_PC, 0, lastpc, ts);
            EmitExport(NEXRV_EXPORT_EVT_SYNC, 0, pc, ts);
          } break;

          case ACT_CAP_DIRECT_DATA: // DAQ_DATA(tag) - always emitted
            EmitExport(NEXRV_EXPORT_EVT_DAQ_DATA, 0, el0, ts);
            break;

          case ACT_CAP_DATA: { // (tag ->) MEMx_N(value, addr omitted)
            uint64_t value = el0, dd = el1, tag = el2;
            unsigned dtype = (unsigned)((dd >> 6) & 0xF), dsize = (unsigned)(dd & 0x3F);
            if (tag != 0) EmitExport(NEXRV_EXPORT_EVT_DAQ_DATA, 0, tag, ts);
            // dsize here is the captured tip dsize (log2 bytes); DaqMemEvt wants bytes.
            EmitExport(DaqMemEvt(dtype, 1u << dsize), NEXRV_EXPORT_ADDR_OMITTED, value, ts);
          } break;

          case ACT_CAP_DADDR: { // (tag ->) MEMx_0(addr)
            uint64_t addr = el0, dd = el1, tag = el2;
            unsigned dtype = (unsigned)((dd >> 6) & 0xF);
            int isWrite = (dtype != DAQ_DTYPE_LOAD);
            if (tag != 0) EmitExport(NEXRV_EXPORT_EVT_DAQ_DATA, 0, tag, ts);
            EmitExport(isWrite ? NEXRV_EXPORT_EVT_MEMWRITE_0 : NEXRV_EXPORT_EVT_MEMREAD_0,
                       addr, 0, ts);
          } break;

          case ACT_CAP_DATA_DADDR: { // (tag ->) MEMx_N(addr, value)
            // el2 = {DirectData[23:0], dtype_dsize[9:0]}
            uint64_t value = el0, addr = el1;
            unsigned dd10 = (unsigned)(el2 & 0x3FF);
            uint64_t tag = (el2 >> 10) & 0xFFFFFF;
            unsigned dtype = (dd10 >> 6) & 0xF, dsize = dd10 & 0x3F;
            if (tag != 0) EmitExport(NEXRV_EXPORT_EVT_DAQ_DATA, 0, tag, ts);
            // dsize is the captured tip dsize (log2 bytes); DaqMemEvt wants bytes.
            EmitExport(DaqMemEvt(dtype, 1u << dsize), addr, value, ts);
          } break;

          case ACT_CAP_IFETCH_TH: case ACT_CAP_DATA_RD_TH:
          case ACT_CAP_DATA_WR:   case ACT_CAP_DATA_RD: {
            // DAQ_COUNTER(count, value2 = [20:19]kind|[18:16]region|[15:0]tag).
            // Only the count is transmitted; kind comes from the command,
            // region/tag (DirectData) are not in the message -> 0.
            unsigned kind = (cmd == ACT_CAP_IFETCH_TH)  ? 0 :
                            (cmd == ACT_CAP_DATA_RD_TH) ? 1 :
                            (cmd == ACT_CAP_DATA_WR)    ? 2 : 3;
            uint64_t v2 = ((uint64_t)kind << 19);
            EmitExport(NEXRV_EXPORT_EVT_DAQ_COUNTER, el0, v2, ts);
          } break;

          case ACT_CAP_CF_SYNC:
            EmitExport(NEXRV_EXPORT_EVT_SYNC, 0, el0, ts);
            break;

          case ACT_CAP_TE:        // tracing-enable control: no trace event
          default:
            break;
        }
      }
      break;

      case NEXUS_TCODE_Ownership:
      break;

      case NEXUS_TCODE_RepeatBranch:  // Handled differently!

      default:
      return -(100 + TCODE);  // Not handled TCODE
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
  int srcPending = 0;
  int skipMessage = 0;

  // Make sure decoder is using real call-stack ...
  if (conf_CallStack < 0)
  {
    conf_CallStack = -conf_CallStack;
  }
  CallStack_Init();

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
        printf(" ERROR: Message with TCODE=%d is not defined for RISC-V\n", tcode);
        return -3;
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
      srcPending = (nexrv_conf_src_bits > 0);
      skipMessage = 0;

      if (disp & 3)
      {
        printf(" TCODE[6]=%d (MSG #%d) - %s", tcode, msgCnt, nexusMsgDef[fldDef - 1].name);
        if (!(disp & 1) || fldBits == 0) printf("\n");
      }
      msgCnt++;

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
    if (srcPending && fldBits >= nexrv_conf_src_bits)
    {
      nexdeco_src = (unsigned int)(fldVal & NexusFieldMask(nexrv_conf_src_bits));
      fldVal >>= nexrv_conf_src_bits;
      WideShr(nexrv_conf_src_bits);
      fldBits -= nexrv_conf_src_bits;
      srcPending = 0;

      if (disp & 1) printf(" SRC[%d]=%u", nexrv_conf_src_bits, nexdeco_src);

      if (nexrv_conf_src_filter >= 0 && (int)nexdeco_src != nexrv_conf_src_filter)
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
      if (disp & 1) printf(" %s[%d]=0x%llx (%llu)\n", nexusMsgDef[fldDef].name, fldBits, fldVal, fldVal);

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

        if (msgFields[0] == NEXUS_TCODE_RepeatBranch)
        {
          // Special handling for repeat branch (which only has 1 field!)
          cnt = msgFields[1]; // Counter set in RepeatBranch message
          msgFieldPos = msgFields[6]; // Restrore previous message (saved)
          msgFieldCnt = msgFields[7];
          msgFields[0] = msgFields[8];
          msgFields[1] = msgFields[9];
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

  if (disp & 4)
  {
    printf("Stat: %d bytes, %d messages, %d error messages", msgBytes, msgCnt, msgErrors);
    if (msgCnt > 0) printf(", %.2lf bytes/message", ((double)msgBytes) / msgCnt);
    if (nInstr > 0) printf(", %d instr, %.3lf bits/instr", nInstr, ((double)msgBytes * 8) / nInstr);
    printf("\n");
  }

  return nInstr; // Number of instructions generated
}

//****************************************************************************
// End of NexRvDeco.c file
