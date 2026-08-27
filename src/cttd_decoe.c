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
// File cttd_decoe.c - RISC-V *E-Trace* (Efficient Trace) te_inst decoder.
//
// This is the E-Trace counterpart to cttd_deco.c (which decodes N-Trace /
// Nexus). It reconstructs the retired PC sequence from a te_inst byte stream
// (the "Efficient Trace for RISC-V" baseline algorithm) and drives the SAME
// CttdExport event bus -> so CTXP export (cttd_ctxp.c) is byte-identical to
// the N-Trace path with no changes to the exporter.
//
// It is a faithful C port of the vendored, cross-validated Siemens reference
// model (riscv-trace-spec referenceFlow decoder_model.py, BSD-2-Clause) with
// two substitutions:
//   * instruction attributes come from the Cttd -pcinfo table (InfoGet)
//     instead of an objdump listing;
//   * report_pc writes the reconstructed PC to -pcout AND emits a CTXP CF
//     event, classified from the same INFO_* flags the N-Trace path uses.
// The Python model stays the conformance oracle (EC2 gate: byte-identical PC
// sequence on all legs).
//
// EC0 seam / event -> CTXP truth table (classification identical to
// cttd_deco.c EmitICNT, lines ~285-295):
//   pcinfo type | INFO_* flags               | CF event
//   ------------+----------------------------+---------------------------
//   BD (taken)  | BRANCH                     | BRANCH_TAKEN   (src->target)
//   BD (n.tkn)  | BRANCH                     | BRANCH_NOTTAKEN(src->src+sz)
//   JD          | JUMP                       | BRANCH_TAKEN   (src->target)
//   CD / CI     | JUMP|CALL [|INDIRECT]      | CALL           (src->target)
//   JI          | JUMP|INDIRECT              | BRANCH_TAKEN   (src->target)
//   R           | JUMP|INDIRECT|RET          | RETURN         (src->target)
//   L           | LINEAR                     | (no event)
// SYNC (F3.0 START / F3.1 TRAP) -> SYNC event is emitted by the sync handler.
//****************************************************************************

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "cttd_info.h"
#include "cttd_export.h"
#include "cttd_daq.h" // shared DAQ (ACT-CAP) -> CTXP mapping

extern FILE *fNex; // te_inst_raw byte stream (opened "rb" by main)

// ---- Build parameters (SSOT mirror of tools/etrace/etrace_common.py and
//      rtl/pkg/ct_pkg.sv CT_ETRACE_*). MVP: RV32, no context/time. ----------
#define E_IADDR_WIDTH 32
#define E_IADDR_LSB    1
#define E_PRIV_WIDTH   3
#define E_ECAUSE_WIDTH 4
// All of bpred/cache/call_counter/return_stack sizes are 0 in the MVP profile,
// so irdepth is 0 bits and context/time fields are absent (nocontext/notime).
#define E_IRDEPTH_BITS 0

#define E_MSB_MASK (1u << (E_IADDR_WIDTH - 1))

// format_t / sync_t / qual_status_t (common/inst_trace.py)
enum { FMT_EXT = 0, FMT_BRANCH = 1, FMT_ADDR = 2, FMT_SYNC = 3 };
enum { SYNC_START = 0, SYNC_TRAP = 1, SYNC_CONTEXT = 2, SYNC_SUPPORT = 3 };
enum { QS_NO_CHANGE = 0, QS_ENDED_REP = 1, QS_TRACE_LOST = 2, QS_ENDED_NTR = 3 };

//============================================================================
// Bit reader over one packet payload (little-endian bit stream with whole-
// packet sign-based decompression). Mirrors common/raw_packet.py: fields are
// read LSB-first; when a field needs more bits than remain, the packet is
// sign-extended from its most-significant received bit.
//============================================================================
typedef struct {
  const unsigned char *p;
  int nbytes;
  int pos;   // next bit index (LSB-first)
  int sign;  // MSB of the payload (extension bit)
} BitRdr;

static void br_init(BitRdr *b, const unsigned char *p, int nbytes)
{
  b->p = p;
  b->nbytes = nbytes;
  b->pos = 0;
  b->sign = (nbytes > 0) ? ((p[nbytes - 1] >> 7) & 1) : 0;
}

static uint64_t br_get(BitRdr *b, int nbits)
{
  uint64_t v = 0;
  int total = b->nbytes * 8;
  for (int k = 0; k < nbits; k++)
  {
    int i = b->pos + k;
    int bit = (i < total) ? ((b->p[i >> 3] >> (i & 7)) & 1) : b->sign;
    v |= (uint64_t)bit << k;
  }
  b->pos += nbits;
  return v;
}

// Sign-extend an n-bit two's-complement value (for delta addresses).
static int64_t twoscomp(uint64_t v, int bits)
{
  if (bits < 64 && (v & (1ULL << (bits - 1))))
    return (int64_t)(v - (1ULL << bits));
  return (int64_t)v;
}

//============================================================================
// te_inst packet (only the baseline fields we parse).
//============================================================================
typedef struct {
  int format;
  int subformat;      // SYNC only
  int has_subformat;
  // common CF fields
  uint32_t address;   // raw field value (pre-shift)
  int has_address;
  int branch;         // SYNC START/TRAP anchor bit
  int branches;       // BRANCH
  uint32_t branch_map;
  int notify, updiscon, irreport;
  uint32_t irdepth;
  // TRAP
  int privilege, ecause, interrupt, thaddr;
  uint32_t tval;
  // SUPPORT
  int ienable, encoder_mode, qual_status, ioptions, denable, dloss, doptions;
  // Format 0 (EXT) optional-mode fields
  int f0_sub;             // F0 subformat (1 bit): 0 = BP, 1 = JTC
  int index;             // F0.1 JTC index
  uint32_t branch_count; // F0.0 predictor-resolved branch count
  int branch_fmt;        // F0.0 branch format
} TeInst;

//============================================================================
// Decoder state (mirror decoder_model.py Decoder).
//============================================================================
typedef struct {
  FILE *out;
  int disp;

  uint32_t pc;
  uint32_t last_pc;
  int have_pc;          // pc validity (before first sync)
  int branches;
  uint64_t branch_map;
  int stop_at_last_branch;
  int inferred_address;
  int start_of_trace;
  uint32_t address;     // reconstructed absolute address target

  // recovered status flags (valid only for ADDR / BRANCH-with-branches)
  int f_notify, f_updiscon, f_irreport;

  // options (from SUPPORT ioptions)
  int opt_implicit_return;
  int opt_full_address;
  int opt_jtc;   // jump-target-cache (F0.1)
  int opt_bp;    // branch-prediction  (F0.0)

  // implicit-return stack (EC5)
  uint32_t ret_stack[64];
  int ret_depth;

  // Format 0 optional-mode mirrors.
  uint32_t jtc[64];       // jump-target cache (F0.1)
  int      jtc_valid[64];
  uint8_t  bp_table[512]; // 2-bit saturating predictors, index iaddr[10:2]
  long     bp_pending;    // predictor-resolved branches still to walk (F0.0)
  int      bp_fail;       // ... plus one inverted (the mispredicted branch)

  long i_count;
  long npackets;
  long ndata;   // te_data (DF) packets
  long ndaq;    // vendor DAQ packets

  // timestamp (MVP: notime -> stays 0; refined in EC4)
  uint64_t cycle;
} EDec;

//============================================================================
// Instruction attribute access via -pcinfo (InfoGet). Substitutes the
// objdump-listing predicates of the reference model.
//============================================================================
typedef struct { unsigned int info; InfoAddr dest; int size; } Instr;

static Instr edec_instr(uint32_t pc)
{
  Instr r;
  InfoAddr dest = 0;
  r.info = InfoGet((InfoAddr)pc, &dest);
  r.dest = dest;
  r.size = (r.info & INFO_4) ? 4 : 2;
  if (r.info == 0)
  {
    fprintf(stderr, "CttdDecoE ERROR: no -pcinfo entry for 0x%08x\n", pc);
    exit(9);
  }
  return r;
}

static int instr_is_branch(const Instr *i)            { return (i->info & INFO_BRANCH) != 0; }
static int instr_is_inferable_jump(const Instr *i)    { return (i->info & INFO_JUMP) && !(i->info & INFO_INDIRECT); }
static int instr_is_uninferable_discon(const Instr *i){ return (i->info & INFO_INDIRECT) != 0; }
static int instr_is_call(const Instr *i)              { return (i->info & INFO_CALL) != 0; }
static int instr_is_return(const Instr *i)            { return (i->info & INFO_RET) != 0; }

//============================================================================
// Reporting: PC to -pcout (oracle format: lowercase hex, no prefix) + CTXP
// control-flow event via the shared export bus.
//============================================================================
static void edec_report_pc(EDec *d, uint32_t pc)
{
  if (d->disp & 1) printf("report_pc[%ld] 0x%x\n", d->i_count, pc);
  fprintf(d->out, "%x\n", pc);
  d->i_count++;
}

// Emit the CF event for the instruction at `src` that produced edge -> `target`.
// `taken` matters only for branches. Classification is by INFO_* flags, exactly
// as cttd_deco.c EmitICNT does, so CTXP matches the N-Trace path byte-for-byte.
static void edec_emit_cf(EDec *d, const Instr *si, uint32_t src, uint32_t target, int taken)
{
  CttdExportEventType evt;
  const char *cmt = NULL;
  if (instr_is_call(si))              { evt = CTTD_EXPORT_EVT_CALL; }
  else if (instr_is_return(si))       { evt = CTTD_EXPORT_EVT_RETURN; }
  else if (instr_is_branch(si) && !taken) { evt = CTTD_EXPORT_EVT_BRANCH_NOTTAKEN; cmt = "no-jump"; }
  else                                { evt = CTTD_EXPORT_EVT_BRANCH_TAKEN; cmt = "jump"; }
  cttd_export_emit_cf(0, evt, src, target, d->cycle, cmt);
}

// JTC index hash (etrace_common.py _jtc_idx).
static unsigned edec_jtc_idx(uint32_t a)
{
  return ((a >> 2) ^ (a >> 8) ^ (a >> 14) ^ (a >> 20)) & 0x3F;
}

// Consume one branch-map bit (baseline: bit 0 == 0 means taken).
static int edec_map_taken(EDec *d, uint32_t src)
{
  if (d->branches == 0)
  { fprintf(stderr, "CttdDecoE ERROR: cannot resolve branch @0x%08x\n", src); exit(9); }
  int t = ((d->branch_map & 1) == 0);
  d->branches -= 1;
  d->branch_map >>= 1;
  return t;
}

// Resolve a conditional branch, honouring branch-prediction (F0.0) when active.
// Mirrors etrace_common.py ExtDecoder.is_taken_branch.
static int edec_is_taken_branch(EDec *d, uint32_t src)
{
  if (!d->opt_bp)
    return edec_map_taken(d, src);

  unsigned idx = (src >> 2) & 0x1FF;
  int ctr = d->bp_table[idx];
  int pred = (ctr >= 2);
  int taken;
  if (d->branches > 0)          // a leftover map bit outranks the F0.0 count
    taken = edec_map_taken(d, src);
  else if (d->bp_pending > 0)   { taken = pred; d->bp_pending -= 1; }
  else if (d->bp_fail)          { taken = !pred; d->bp_fail = 0; }
  else                          taken = edec_map_taken(d, src);
  d->bp_table[idx] = (uint8_t)(taken ? (ctr < 3 ? ctr + 1 : 3)
                                     : (ctr > 0 ? ctr - 1 : 0));
  return taken;
}

//============================================================================
// next_pc: advance one retired instruction (decoder_model.py next_pc).
// Returns non-zero when the walk must stop at a reported (uninferable) target.
//============================================================================
static int edec_next_pc(EDec *d, uint32_t address)
{
  Instr instr = edec_instr(d->pc);
  uint32_t this_pc = d->pc;
  int stop_here = 0;
  int fold = 0;

  if (instr_is_inferable_jump(&instr))
  {
    d->pc = (uint32_t)instr.dest;                 // direct jal/c.j/c.jal (or direct call)
    edec_emit_cf(d, &instr, this_pc, d->pc, 1);
  }
  else if (d->opt_implicit_return && instr_is_return(&instr) && d->ret_depth > 0)
  {
    d->pc = d->ret_stack[--d->ret_depth];         // implicit return (EC5)
    fold = 1;
    edec_emit_cf(d, &instr, this_pc, d->pc, 1);
  }
  else if (instr_is_uninferable_discon(&instr))
  {
    d->pc = address;                              // target from reported address
    stop_here = 1;
    edec_emit_cf(d, &instr, this_pc, d->pc, 1);
  }
  else if (instr_is_branch(&instr))
  {
    int taken = edec_is_taken_branch(d, this_pc);
    if (taken) d->pc = (uint32_t)instr.dest;
    else       d->pc = this_pc + instr.size;
    edec_emit_cf(d, &instr, this_pc, d->pc, taken);
  }
  else
  {
    d->pc += instr.size;                          // linear (no CF event)
  }

  // Return-address stack push at every call (for implicit-return mirror).
  if (d->opt_implicit_return && instr_is_call(&instr) && d->ret_depth < (int)(sizeof(d->ret_stack)/sizeof(d->ret_stack[0])))
    d->ret_stack[d->ret_depth++] = this_pc + instr.size;

  // JTC install (F0.1 mirror): cache every uninferable-discontinuity target we
  // walk to, so a later F0.1 packet can replay it from the index alone.
  // Folded returns are excluded: the encoder installs only address-REPORTED
  // targets (etrace_common.py ExtDecoder.next_pc keeps the same rule).
  if (stop_here && !fold && d->opt_jtc && d->pc == address && instr_is_uninferable_discon(&instr))
  {
    unsigned ji = edec_jtc_idx(address);
    d->jtc[ji] = address;
    d->jtc_valid[ji] = 1;
  }

  // Implicit-return fold arriving AT the reported address ends the walk like
  // an explicit discontinuity arrival: the fold IS an uninferable
  // discontinuity, merely silent on the wire -- without this a flush whose
  // target is reached by a fold walks straight past the flush point (P10
  // soak S-1 family). Guarded on an exhausted branch map: an interior fold
  // touching the address while bits are pending is not the packet endpoint.
  if (fold && d->pc == address && !d->stop_at_last_branch)
  {
    Instr cur = edec_instr(d->pc);
    int limit = instr_is_branch(&cur) ? 1 : 0;
    if (d->branches <= limit)
      stop_here = 1;
  }

  d->last_pc = this_pc;
  return stop_here;
}

//============================================================================
// follow_execution_path (decoder_model.py follow_execution_path).
//============================================================================
static void edec_follow(EDec *d, uint32_t address, int is_sync)
{
  uint32_t local_prev = d->pc;
  for (;;)
  {
    if (d->inferred_address)
    {
      int stop = edec_next_pc(d, local_prev);
      edec_report_pc(d, d->pc);
      if (stop) d->inferred_address = 0;
    }
    else
    {
      int stop = edec_next_pc(d, address);
      edec_report_pc(d, d->pc);

      Instr cur = edec_instr(d->pc);
      int branch_limit = instr_is_branch(&cur) ? 1 : 0;

      if (d->branches == 1 && instr_is_branch(&cur) && d->stop_at_last_branch)
      {
        d->stop_at_last_branch = 0;
        return;
      }
      if (stop)
      {
        // reached reported addr after uninferable discontinuity
        return;
      }
      if (!is_sync && d->pc == address && !d->stop_at_last_branch &&
          d->f_notify && d->branches == branch_limit)
      {
        return;
      }
      {
        Instr lasti = edec_instr(d->last_pc);
        if (!is_sync && d->pc == address && !d->stop_at_last_branch &&
            !instr_is_uninferable_discon(&lasti) && !d->f_updiscon &&
            d->branches == branch_limit &&
            (!d->f_irreport || (int)0 == 0 /* irdepth==irstack_depth, both 0 in MVP */))
        {
          d->inferred_address = 1;
          return;
        }
      }
      if (is_sync && d->pc == address && d->branches == branch_limit)
      {
        return;
      }
    }
  }
}

//============================================================================
// Status-field decompression (decoder_model.py recover_status_fields).
//============================================================================
static void edec_recover_status(EDec *d, const TeInst *t)
{
  d->f_notify = 0; d->f_updiscon = 0; d->f_irreport = 0;
  // F0.0 with address (branch_fmt 2/3) carries the same trailer -- the
  // vendored guard only covered ADDR/BRANCH (etrace_common.py decompresses
  // it in _process_f00 for the same reason).
  if (t->format == FMT_ADDR || (t->format == FMT_BRANCH && t->branches != 0)
      || (t->format == FMT_EXT && t->f0_sub == 0
          && (t->branch_fmt == 2 || t->branch_fmt == 3)))
  {
    int msb = ((E_MSB_MASK & (t->address << E_IADDR_LSB)) == 0) ? 0 : 1;
    d->f_notify   = (t->notify != msb);
    d->f_updiscon = (t->updiscon != t->notify);
    d->f_irreport = (t->irreport != t->updiscon);
  }
}

//============================================================================
// SUPPORT packet -> options (decoder_model.py process_support).
//============================================================================
static void edec_process_support(EDec *d, const TeInst *t)
{
  // ioptions bits: 0 implicit-return, 2 full-address, 3 jump-target-cache,
  // 4 branch-prediction (decoder_model.py process_support order).
  d->opt_implicit_return = (t->ioptions & 1) != 0;
  d->opt_full_address    = (t->ioptions >> 2) & 1;
  d->opt_jtc             = (t->ioptions >> 3) & 1;
  d->opt_bp              = (t->ioptions >> 4) & 1;
  if (t->qual_status != QS_NO_CHANGE)
  {
    d->start_of_trace = 1;   // trace ended, ready to re-start
    // Cold restart of the F0 mirrors (ExtDecoder.process_support).
    for (int i = 0; i < 64; i++) d->jtc_valid[i] = 0;
    for (int i = 0; i < 512; i++) d->bp_table[i] = 1;
    d->bp_pending = 0;
    d->bp_fail = 0;
    // Implicit-return mirror: the composer empties its return stack at the
    // trace-off edge and at the FIFO_OVERRUN anchor -- both reach the wire
    // as exactly this packet. A stale frame here would fold a post-recovery
    // return the encoder reports explicitly (P10 soak S-1 family).
    d->ret_depth = 0;
  }
}

//============================================================================
// Format 0 processing (etrace_common.py ExtDecoder process_te_inst/_process_f00).
//============================================================================
static void edec_process_f0(EDec *d, const TeInst *t)
{
  if (t->f0_sub == 1)   // F0.1: absolute target from the mirrored JTC, then F2 walk
  {
    if (!d->jtc_valid[t->index])
    { fprintf(stderr, "CttdDecoE ERROR: JTC index %d not in decoder cache\n", t->index); exit(9); }
    // A cache hit IS a discontinuity-target report (entries are only ever
    // installed at reported updiscon targets) and the packet carries no
    // notify/updiscon trailer: walk with updiscon semantics, same as the
    // encoder signals explicitly on F1/F2/F0.0 target reports.
    d->f_notify = 0;
    d->f_updiscon = 1;
    d->f_irreport = 0;
    d->stop_at_last_branch = 0;
    d->address = d->jtc[t->index];
    if (t->branches)
    {
      d->branch_map |= ((uint64_t)t->branch_map << d->branches);
      d->branches += t->branches;
    }
    edec_follow(d, d->address, 0);
    return;
  }

  // F0.0: predictor-resolved branches.
  if (!d->opt_bp)
  { fprintf(stderr, "CttdDecoE ERROR: F0.0 packet but branch-prediction not announced\n"); exit(9); }
  uint32_t count = t->branch_count + 31;
  if (t->branch_fmt == 0)          // no address: walk count predicted + one failed
  {
    d->bp_pending += count;
    d->bp_fail = 1;
    while (d->bp_pending || d->bp_fail)
    {
      int stop = edec_next_pc(d, 0);
      edec_report_pc(d, d->pc);
      if (stop) { fprintf(stderr, "CttdDecoE ERROR: F0.0 walk hit uninferable discontinuity\n"); exit(9); }
    }
    return;
  }
  if (t->branch_fmt == 2)          // with address: F2 walk with predicted branches
  {
    d->bp_pending += count;
    d->stop_at_last_branch = 0;
    uint32_t addr = t->address << E_IADDR_LSB;
    if (d->opt_full_address) d->address = addr;
    else d->address = (uint32_t)((int64_t)d->address + twoscomp(addr, E_IADDR_WIDTH));
    edec_follow(d, d->address, 0);
    return;
  }
  fprintf(stderr, "CttdDecoE ERROR: F0.0 branch_fmt=3 (addr-fail) unsupported\n"); exit(9);
}

//============================================================================
// Main te_inst processing (decoder_model.py process_te_inst).
//============================================================================
static void edec_process(EDec *d, const TeInst *t)
{
  if (t->format == FMT_EXT) { edec_process_f0(d, t); return; }

  if (t->format == FMT_SYNC)
  {
    uint32_t prev_pc = d->pc; // last retired PC before this anchor (interrupted PC)
    if (t->subformat == SYNC_SUPPORT) { edec_process_support(d, t); return; }
    if (t->subformat == SYNC_CONTEXT) { return; }
    if (t->subformat == SYNC_TRAP)
    {
      // report_trap / report_epc are debug-only in the model; no PC/CTXP effect.
      // Sideband (ecause/tval/priv/interrupt) is extracted by parse_sync; its
      // correctness is validated transitively by EC2 (a wrong field width would
      // desync the bit stream and corrupt every subsequent PC).
      if (d->disp & 1)
        printf("TRAP ecause=%d interrupt=%d thaddr=%d priv=%d tval=0x%x\n",
               t->ecause, t->interrupt, t->thaddr, t->privilege, t->tval);
      if (t->thaddr == 0) return;
    }

    d->inferred_address = 0;
    d->address = t->address << E_IADDR_LSB;
    if (t->subformat == SYNC_TRAP || d->start_of_trace)
    {
      d->branches = 0;
      d->branch_map = 0;
    }
    {
      Instr ai = edec_instr(d->address);
      if (instr_is_branch(&ai))
      {
        d->branch_map |= ((uint64_t)t->branch << d->branches);
        d->branches += 1;
      }
    }
    // Emit a CTXP anchor record. An interrupt trap becomes an INTERRUPT record
    // (interrupted PC -> handler); every other anchor (START / exception trap)
    // a SYNC. Record addresses come from the EC2-validated reconstruction.
    if (t->subformat == SYNC_TRAP && t->interrupt == 1)
      cttd_export_emit_cf(0, CTTD_EXPORT_EVT_INTERRUPT, prev_pc, d->address, d->cycle, NULL);
    else
      cttd_export_emit_cf(0, CTTD_EXPORT_EVT_SYNC, 0, d->address, d->cycle, NULL);

    if (t->subformat == SYNC_START && !d->start_of_trace)
    {
      edec_follow(d, d->address, 1);
    }
    else
    {
      d->pc = d->address;
      d->have_pc = 1;
      edec_report_pc(d, d->pc);
      d->last_pc = d->pc;
    }
    d->start_of_trace = 0;
  }
  else
  {
    if (t->format == FMT_ADDR || t->branches != 0)
    {
      d->stop_at_last_branch = 0;
      uint32_t address = t->address << E_IADDR_LSB;
      if (d->opt_full_address)
        d->address = address;
      else
        d->address = (uint32_t)((int64_t)d->address + twoscomp(address, E_IADDR_WIDTH));
    }
    if (t->format == FMT_BRANCH)
    {
      d->stop_at_last_branch = (t->branches == 0);
      d->branch_map |= ((uint64_t)t->branch_map << d->branches);
      if (t->branches == 0) d->branches += 31;
      else                  d->branches += t->branches;
    }
    edec_follow(d, d->address, 0);
  }
}

//============================================================================
// Packet parsing (decoder_model.py create_sync/create_addr/create_branch).
//============================================================================
static void parse_sync(BitRdr *b, TeInst *t)
{
  t->subformat = (int)br_get(b, 2);
  t->has_subformat = 1;
  int addr_bits = E_IADDR_WIDTH - E_IADDR_LSB;
  if (t->subformat == SYNC_START)
  {
    t->branch    = (int)br_get(b, 1);
    t->privilege = (int)br_get(b, E_PRIV_WIDTH);
    // time (0 bits, notime), context (0 bits, nocontext)
    t->address   = (uint32_t)br_get(b, addr_bits);
    t->has_address = 1;
  }
  else if (t->subformat == SYNC_TRAP)
  {
    t->branch    = (int)br_get(b, 1);
    t->privilege = (int)br_get(b, E_PRIV_WIDTH);
    t->ecause    = (int)br_get(b, E_ECAUSE_WIDTH);
    t->interrupt = (int)br_get(b, 1);
    t->thaddr    = (int)br_get(b, 1);
    t->address   = (uint32_t)br_get(b, addr_bits);
    t->has_address = 1;
    if (t->interrupt != 1)
      t->tval = (uint32_t)br_get(b, E_IADDR_WIDTH);
  }
  else if (t->subformat == SYNC_CONTEXT)
  {
    // privilege/time/context only (context/time 0 bits in MVP)
    t->privilege = (int)br_get(b, E_PRIV_WIDTH);
  }
  else // SYNC_SUPPORT
  {
    t->ienable      = (int)br_get(b, 1);
    t->encoder_mode = (int)br_get(b, 1);
    t->qual_status  = (int)br_get(b, 2);
    t->ioptions     = (int)br_get(b, 5);
    t->denable      = (int)br_get(b, 1);
    t->dloss        = (int)br_get(b, 1);
    t->doptions     = (int)br_get(b, 4);
  }
}

static void parse_addr(BitRdr *b, TeInst *t)
{
  int addr_bits = E_IADDR_WIDTH - E_IADDR_LSB;
  t->address  = (uint32_t)br_get(b, addr_bits);
  t->has_address = 1;
  t->notify   = (int)br_get(b, 1);
  t->updiscon = (int)br_get(b, 1);
  t->irreport = (int)br_get(b, 1);
  t->irdepth  = (uint32_t)br_get(b, E_IRDEPTH_BITS);
}

static void parse_branch(BitRdr *b, TeInst *t)
{
  int addr_bits = E_IADDR_WIDTH - E_IADDR_LSB;
  t->branches = (int)br_get(b, 5);
  int branch_bits; int has_address;
  int n = t->branches;
  if (n == 0)      { branch_bits = 31; has_address = 0; }
  else if (n == 1) { branch_bits = 1;  has_address = 1; }
  else if (n <= 3) { branch_bits = 3;  has_address = 1; }
  else if (n <= 7) { branch_bits = 7;  has_address = 1; }
  else if (n <= 15){ branch_bits = 15; has_address = 1; }
  else             { branch_bits = 31; has_address = 1; }
  t->branch_map = (uint32_t)br_get(b, branch_bits);
  if (has_address)
  {
    t->address  = (uint32_t)br_get(b, addr_bits);
    t->has_address = 1;
    t->notify   = (int)br_get(b, 1);
    t->updiscon = (int)br_get(b, 1);
    t->irreport = (int)br_get(b, 1);
    t->irdepth  = (uint32_t)br_get(b, E_IRDEPTH_BITS);
  }
}

// Format 0 (EXT) optional-mode packets (etrace_common.py ExtDecoder.create_te_inst).
static void parse_f0(BitRdr *b, TeInst *t)
{
  t->f0_sub = (int)br_get(b, 1);
  if (t->f0_sub == 1)               // F0.1 (jump-target-cache)
  {
    t->index    = (int)br_get(b, 6);
    t->branches = (int)br_get(b, 5);
    if (t->branches != 0)
    {
      int n = t->branches;
      int bits = (n <= 1) ? 1 : (n <= 3) ? 3 : (n <= 7) ? 7 : (n <= 15) ? 15 : 31;
      t->branch_map = (uint32_t)br_get(b, bits);
    }
    t->irreport = (int)br_get(b, 1);
  }
  else                              // F0.0 (branch-prediction)
  {
    t->branch_count = (uint32_t)br_get(b, 32);
    t->branch_fmt   = (int)br_get(b, 2);
    if (t->branch_fmt == 2 || t->branch_fmt == 3)
    {
      int addr_bits = E_IADDR_WIDTH - E_IADDR_LSB;
      t->address  = (uint32_t)br_get(b, addr_bits);
      t->has_address = 1;
      t->notify   = (int)br_get(b, 1);
      t->updiscon = (int)br_get(b, 1);
      t->irreport = (int)br_get(b, 1);
    }
  }
}

static void parse_packet(BitRdr *b, TeInst *t)
{
  memset(t, 0, sizeof(*t));
  t->format = (int)br_get(b, 2);
  if (t->format == FMT_SYNC)        parse_sync(b, t);
  else if (t->format == FMT_ADDR)   parse_addr(b, t);
  else if (t->format == FMT_BRANCH) parse_branch(b, t);
  else                              parse_f0(b, t);
}

//============================================================================
// DF: te_data packet (data trace) -> CTXP MEMREAD/MEMWRITE.
// Mirrors tools/etrace/etrace_common.py parse_te_data: Unified-L/S, format bit
// 1 = store, bit 0 = unaligned; size-dependent data_len field; size-based
// address reconstruction; data sign-extended to the access width.
//============================================================================
static void edec_df_packet(BitRdr *b, EDec *d)
{
  int fmt = (int)br_get(b, 2);
  int sz  = (int)br_get(b, 2);
  (void)br_get(b, 2);                       // diff (unused: aligned data trace)
  int dl  = sz ? (int)br_get(b, sz) : 0;
  int w   = 8 * (dl + 1);
  uint64_t v = br_get(b, w);
  uint32_t a = (uint32_t)br_get(b, 32);

  unsigned nbytes = 1u << sz;
  uint64_t accmask = (nbytes >= 8) ? ~0ULL : ((1ULL << (8 * nbytes)) - 1);
  if ((v >> (w - 1)) & 1)                   // sign-extend to access width
  {
    uint64_t wmask = (w >= 64) ? ~0ULL : ((1ULL << w) - 1);
    v |= (accmask ^ wmask);
  }
  int is_store = (fmt & 2) != 0;
  uint32_t addr = (fmt & 1) ? a : (uint32_t)(((uint64_t)a << sz) & 0xFFFFFFFF);

  CttdExportEvent e;
  e.source_id   = 0;
  e.type        = DaqMemEvt(is_store ? 1 : DAQ_DTYPE_LOAD, nbytes);
  e.value1      = addr;
  e.value2      = v & accmask;
  e.cycle_count = d->cycle;
  e.comment     = NULL;
  cttd_export_emit(&e);
}

//============================================================================
// DAQ: vendor msg_type-1 packet -> CTXP via the shared cttd_daq_emit mapping.
// Wire (etrace_common.py parse_daq): idtag[8] + data[192] (three 64-bit
// elements, element 0 in the LSBs), whole-packet sign-compressed.
//============================================================================
static void edec_daq_packet(BitRdr *b, EDec *d)
{
  unsigned idtag = (unsigned)br_get(b, 8);
  uint64_t el0 = br_get(b, 64);
  uint64_t el1 = br_get(b, 64);
  uint64_t el2 = br_get(b, 64);
  cttd_daq_emit(0, idtag, el0, el1, el2, d->cycle);
}

//============================================================================
// Entry point (called from main for -decoe). Reads the te_inst_raw stream
// from fNex, decodes to -pcout (d->out) and drives the export bus.
// Returns number of reported PCs (>0) on success, <=0 on error.
//============================================================================
int EtraceDeco(FILE *fOut, int disp)
{
  // Slurp the whole stream.
  fseek(fNex, 0, SEEK_END);
  long sz = ftell(fNex);
  fseek(fNex, 0, SEEK_SET);
  if (sz <= 0) return -1;
  unsigned char *buf = (unsigned char *)malloc((size_t)sz);
  if (buf == NULL) return -2;
  if (fread(buf, 1, (size_t)sz, fNex) != (size_t)sz) { free(buf); return -3; }

  EDec d;
  memset(&d, 0, sizeof(d));
  d.out = fOut;
  d.disp = disp;
  d.start_of_trace = 1;
  for (int i = 0; i < 512; i++) d.bp_table[i] = 1; // weakly-not-taken init

  long start = 0;
  while (start < sz)
  {
    unsigned int header = buf[start];
    int plen = header & 0x1F;         // payload length in bytes
    int mtype = (header >> 5) & 0x3;  // 2 = TE_INST
    if (start + 1 + plen > sz)
    {
      fprintf(stderr, "CttdDecoE ERROR: truncated packet at byte %ld\n", start);
      free(buf);
      return -4;
    }
    BitRdr br;
    br_init(&br, buf + start + 1, plen);
    if (mtype == 2)         // TE_INST: instruction / control flow
    {
      TeInst t;
      parse_packet(&br, &t);
      d.npackets++;
      if (disp & 1) printf("---- te_inst#%ld fmt=%d ----\n", d.npackets, t.format);
      edec_recover_status(&d, &t);
      edec_process(&d, &t);
    }
    else if (mtype == 3)    // TE_DATA: data trace (DF) -> MEMREAD/MEMWRITE
    {
      edec_df_packet(&br, &d);
      d.ndata++;
    }
    else if (mtype == 1)    // vendor DAQ -> DAQ_*/MEM* via shared mapping
    {
      edec_daq_packet(&br, &d);
      d.ndaq++;
    }
    // else: unknown msg_type -> skip.
    start += plen + 1;
  }

  free(buf);
  printf("E-Trace decode: %ld te_inst / %ld te_data / %ld daq packets, %ld PCs\n",
         d.npackets, d.ndata, d.ndaq, d.i_count);
  return (d.i_count > 0) ? (int)d.i_count : -5;
}

//****************************************************************************
// End of cttd_decoe.c file
