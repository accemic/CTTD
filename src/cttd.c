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
// File cttd.c  - Nexus Trace dump/encode/decode reference implementation
//                (CTTD; derived from the NexRv reference code, file nexrv.c)

// Version printed by the banner. The makefile passes -DCTTD_VERSION from
// `git describe --tags`; this default only applies to a build from a source
// archive without git, and names the last tagged release the sources are
// known to be at or past.
#ifndef CTTD_VERSION
#define CTTD_VERSION "v2.3.0-cttd3+"
#endif

// Code below is written in plain C-code.
// It was compiled using VisualC, GNU and IAR C/C++ compiler.
//  1. Only standard C-types are used.
//  2. Only few standard C functions used - see notes with "#include <...>"
//  3. Only non K&R C is 'for (int x' and 'int x;' between instructions.

#include <stdio.h>  //  For NULL, 'printf', 'fopen, ...'
#include <stdlib.h> //  For 'exit'
#include <string.h> //  For 'strcmp', 'strchr'
#include <ctype.h>  //  For 'isspace'
#include <inttypes.h>   //  For print formats PRIX64

#include "cttd.h"      // For Nexus_TypeAddr
#include "cttd_info.h"  // For 'InfoInit/InfoTerm'
#include "cttd_target.h" // Multi-target (multi-SRC) decode contexts

// #define WITH_EXT 1      // Enable (in code, not by -DWITH_EXT=1 command line)

#ifndef WITH_EXT
#define WITH_EXT 0      // By default without QEMU trace handling
#endif

FILE *fNex  = NULL;     // Used by NexusDump/NexusDeco/NexusEnco
int cttd_conf_src_bits = 0;
int cttd_conf_src_filter = -1;
int cttd_conf_src_value = 0;

// Address text width of the ACTIVE trace source (R1.2 / X2c, per-target since
// R1.2b), see cttd.h. Set ONLY by the TCODE-58 handler from CAPS bit 23
// (ADDR64) -- deliberately not reachable from the command line -- and swapped
// per source by NexusDecoStateSave/Restore, like conf_BranchPredict below.
int cttd_conf_addr64 = 0;

int conf_Repeat = 0;        // 0=no repeat, 1=releat branch only, 2=repeat history

// Accemic VendorBP (TCODE 56): decode with the branch-prediction model
// (-bp with -deco). Off by default -- BP-less captures decode byte-identically.
int conf_BranchPredict = 0;

// Accemic VendorConfig (TCODE 58) autoconfig override tracking: an explicitly
// given CLI flag wins over the in-band config message (SPEC section 5). Set
// by the argument parser, read by the TCODE-58 handler in cttd_deco.c.
int conf_BranchPredictCli = 0;  // -bp given explicitly
int cttd_conf_src_bits_cli = 0; // -src given explicitly

// P3 DF address compression (TCODE 13/14 + XOR'd 5/6 DADDR): decode-side
// mode switch. Enabled by -dfxor, by TCODE-58 autoconfig (ENAB.21), or by
// stream evidence (a decoded 13/14) -- all enable-only, no disable path
// (see the extern block in cttd_deco.c for the full priority note). The
// explicit flag exists because data-only streams carry NO config message
// (their first evidence is the leading 13/14 itself).
int conf_DfXor = 0;
int conf_DfXorCli = 0;          // -dfxor given explicitly

// Accemic sijump: -conv -objd ... -sijump classifies statically inferable
// jalr forms (adjacent auipc/lui pair to rs1, or rs1=x0) as JD/CD WITH a
// computed target, so the decoder can walk encoder-folded sequential jumps.
// Off by default -- the generated pcinfo is unchanged without the flag.
int conf_SijumpConv = 0;

#if 1 // Callstack related

#define CALLSTACK_MAX 32    // Max depth
_Static_assert(CALLSTACK_MAX == CTTD_CALLSTACK_MAX, "cttd_target.h mirror out of sync");

int conf_CallStack = 0;     // =0: No support for call stack
// int conf_CallStack = -8;    // <0: Callstack without a stack (just a counter). Max -N entries.
// int conf_CallStack = 8;     // >0: Call-stack 'N' entries deep (with storing of an address).

// W4: a return whose EXPLICIT trace target differs from the mirror's prediction
// is a legitimate event (context switch, longjmp, mirror overrun), not a stream
// defect -- see the long rationale at CallStackReanchor() in cttd_deco.c. The
// decoder therefore re-anchors and keeps going. This flag restores the historic
// `exit(EXIT_FAILURE)` for single-task programs, where the mismatch really is a
// strong desynchronization signal.
int conf_CallStackStrict = 0;

Nexus_TypeAddr callStack[CALLSTACK_MAX] = {0};
int callStackTop = 0;
int callStackCnt = 0;
int callStackMax = 0;

void CallStack_Init()
{
  callStackCnt = 0;
  callStackTop = 0;
  callStack[0] = 0; // Needed for logging with 'no-stack'
  if (conf_CallStack >= 0)
  {
    callStackMax = conf_CallStack;
  }
  else
  {
    callStackMax = -conf_CallStack;
  }
  // The ring has CALLSTACK_MAX slots. A larger configuration must be capped
  // here, otherwise push/pop index past the end of the array.
  if (callStackMax > CALLSTACK_MAX) callStackMax = CALLSTACK_MAX;
}

void CallStack_Push(Nexus_TypeAddr ret)
{
  if (getenv("NEXRV_CSDBG")) printf("[CS] Push[%d] 0x%" PRIX64 "\n", callStackCnt + 1, ret);

  if (conf_CallStack <= 0)
  {
    // Callstack without storing addresses (just saturating +- counter)
    if (callStackCnt < callStackMax)
    {
      callStackCnt++; // Count (saturating at callStackMax)
    }

    return;
  }

  // Adjust 'top' (with wrap-around).
  //
  // FIX (2026-07-28): the condition was `callStackTop >= callStackMax`, which
  // wrapped only AFTER the last valid index. With callStackTop ==
  // callStackMax-1 (i.e. 31), 31 >= 32 is false -> callStackTop becomes 32 ->
  // `callStack[32]` writes past the array. Pop already wraps correctly at
  // callStackMax-1, so the ring is 0..callStackMax-1; push must do the same.
  //
  // Found on the Linux boot of the CVA6: its OpenSBI phase nests deeply enough
  // to fill the ring -- no earlier test program ever did. On x86 the overflow
  // happened to hit the neighbouring global `conf_CallStack` and went
  // unnoticed; on aarch64 (the board) the same decode aborted after 3 282
  // instead of 53 219 instructions, and without optimisation with a segfault.
  // Confirmed with AddressSanitizer: "global-buffer-overflow ... CallStack_Push
  // src/cttd.c:104 ... 0 bytes to the right of global variable 'callStack'".
  if (callStackTop + 1 >= callStackMax)
  {
    callStackTop = 0; // Wrap-around
  }
  else
  {
    callStackTop++;   // Just next
  }

  //Store (in new top)
  callStack[callStackTop] = ret;

  // Calculate new size (saturating)
  if (callStackCnt < callStackMax)
  {
    callStackCnt++;
  }
}

Nexus_TypeAddr CallStack_Pop()
{
  if (getenv("NEXRV_CSDBG")) printf("[CS] Pop[%d] 0x%" PRIX64 "\n", callStackCnt, callStack[callStackTop]);

  // Calculate new size (and handle empty)
  if (callStackCnt == 0) return 1;  // Empty ('1' will never match 'real PC'!
  callStackCnt--;

  if (conf_CallStack <= 0)
  {
    return 0; // Any non-empty address (it will NOT be compared)
  }

  int prevTop = callStackTop;

  // Adjust 'top' (with wrap-around)
  if (callStackTop == 0)
  {
    callStackTop = (callStackMax - 1);  // Wrap around
  }
  else
  {
    callStackTop--;                     // Just previous
  }

  return callStack[prevTop];  // Return element on top (before adjustment)
}

#endif

#if 1 // Multi-target (multi-SRC) decode contexts

CttdTarget cttd_targets[CTTD_TARGET_MAX];
int cttd_target_mode = 0;
int cttd_target_active = -1;

static unsigned long long cttd_target_unknown_warned = 0; // warn-once bitmask

void CallStackStateInit(CttdCallStackState *s)
{
  s->cnt = 0;
  s->top = 0;
  s->stack[0] = 0;
}

void CallStackStateSave(CttdCallStackState *s)
{
  for (int i = 0; i < CTTD_CALLSTACK_MAX; i++) s->stack[i] = callStack[i];
  s->top = callStackTop;
  s->cnt = callStackCnt;
}

void CallStackStateRestore(const CttdCallStackState *s)
{
  for (int i = 0; i < CTTD_CALLSTACK_MAX; i++) callStack[i] = s->stack[i];
  callStackTop = s->top;
  callStackCnt = s->cnt;
}

int CttdTargetSwitch(unsigned int src)
{
  if (!cttd_target_mode) return 0;
  if (cttd_target_active >= 0 && (unsigned int)cttd_target_active == src) return 0;

  if (src >= CTTD_TARGET_MAX || !cttd_targets[src].used)
  {
    if (src < 64 && !(cttd_target_unknown_warned & (1ull << src)))
    {
      printf("WARNING: message SRC=%u has no configured -target, skipping its messages\n", src);
      cttd_target_unknown_warned |= (1ull << src);
    }
    return -1;
  }

  if (cttd_target_active >= 0)
  {
    CttdTarget *cur = &cttd_targets[cttd_target_active];
    NexusDecoStateSave(&cur->deco);
    CallStackStateSave(&cur->cs);
    InfoDetach(&cur->info);
  }

  CttdTarget *nxt = &cttd_targets[src];
  NexusDecoStateRestore(&nxt->deco);
  CallStackStateRestore(&nxt->cs);
  InfoAttach(&nxt->info);
  cttd_target_active = (int)src;
  return 0;
}

FILE *CttdTargetActiveOut(FILE *fallback)
{
  if (!cttd_target_mode || cttd_target_active < 0) return fallback;
  return cttd_targets[cttd_target_active].fOut;
}

void CttdTargetFlushActive(void)
{
  if (!cttd_target_mode || cttd_target_active < 0) return;
  CttdTarget *cur = &cttd_targets[cttd_target_active];
  NexusDecoStateSave(&cur->deco);
  CallStackStateSave(&cur->cs);
  InfoDetach(&cur->info);
  cttd_target_active = -1;
}

#endif

extern int NexusDump(FILE *f, int disp);
extern int NexusDeco(FILE *f, int disp);
extern int EtraceDeco(FILE *f, int disp); // E-Trace (te_inst) decode -> pcout + CTXP
extern int NexusEnco(FILE *f, int level, int disp);
extern int ConvGnuObjdump(FILE *fObjd, FILE *fPcInfo);
extern int ConvAddInfo(FILE *fIn, FILE *fOut, FILE *fComp);
extern int ConvRtlTrace(FILE *fIn, FILE *fOut);
#if WITH_EXT
extern int ExtProcess(int argc, char *argv[]); // Optional external/QEMU trace handler
#endif

static int error(const char *err)
{
  printf("\n");
  printf("Cttd ERROR: %s\n", err);
  return 9;
}

static int InitRuntimeConfig(void)
{
  const char *env = getenv("NEXRV_SRC_BITS");
  if (env != NULL && env[0] != '\0')
  {
    char *end = NULL;
    long value = strtol(env, &end, 10);
    while (end != NULL && isspace((unsigned char)*end)) end++;

    if (end == env || (end != NULL && *end != '\0') || value < 0 || value > 6)
    {
      printf("\n");
      printf("Cttd ERROR: Invalid NEXRV_SRC_BITS value '%s' (expected 0-6)\n", env);
      return 9;
    }

    cttd_conf_src_bits = (int)value;
  }

  return 0;
}

static int usage(const char *err)
{
  if (err != NULL)
  {
    error(err);
  }
  printf("\n");
  printf("CEDARtools.TraceDecoder (CTTD) %s\n", CTTD_VERSION);
  printf("  derived from NexRv, the RISC-V Nexus Trace TG reference code\n");
  printf("  (c) 2020 IAR Systems AB, (c) 2026 Accemic Technologies GmbH -- ISC\n");
  printf("Usage:\n");
  printf("  Cttd -dump <nex> [<dump>] [-msg|-none] - dump Nexus file\n");
  printf("  Cttd -deco <nex> -pcinfo <info> -pcout <pco> [-stat|-full|-all|-msg|-none] - decode trace\n");
  printf("  Cttd -decoe <te_inst_raw> -pcinfo <info> -pcout <pco> [-stat|-full|-all|-msg|-none] - decode E-Trace\n");
  printf("  Cttd -enco <pcseq> -nex <nex> [-nobhm|-norbm|-cs [<cs>]|-rpt <m>] [-stat|-full|-all|-msg|-none] - encode trace \n");
  printf("  Cttd -conv -objd <objd> -pcinfo <pci> [-sijump] - create <pci> from objdump -d output <objd>\n");
  printf("                                 (-sijump: fold auipc/lui+jalr into sequentially-implicit jumps, CTTE CT_SIJUMP cores)\n");
  printf("  Cttd -conv -pcinfo <pci> -pconly <pco> -pcseq <pcs> - convert <pco> to <pcs> using <pci>\n");
  printf("  Cttd -conv -rtl <rtl> -pconly <pco> - create <pco> from RTL/ISS trace file\n");
  printf("  Cttd -diff <-pcseq|-pconly> <pcs|pco> -pcout <pco> - compare against decoded -pcout\n");
#if WITH_EXT
  printf("  Cttd -ext ... - extra processing (use -ext only to display extra usage)\n");
#endif
  printf("where:\n");
  printf("  -nobhm|-norbm               - do not generate Branch History/Repeat Branch Messages\n");
  printf("  -cs [<cs>]                  - enable call-stack level <cs> (0=none, 8 is default)\n");
  printf("  -cs <cs>                     - (-deco) return-address mirror depth, 0-%d (default %d)\n", CALLSTACK_MAX, CALLSTACK_MAX);
  printf("                                 0 turns the mirror off -- only valid for streams\n");
  printf("                                 WITHOUT implicit-return compression (CAPS/ENAB bit 0)\n");
  printf("  -csstrict                    - (-deco) treat a return whose explicit trace target\n");
  printf("                                 differs from the mirror as a fatal error again\n");
  printf("                                 (default: visible re-anchor, decode continues)\n");
  printf("  -rpt [<m>]                  - enable repeat detection (0=none,1=repeat branch,2=repeat history)\n");
  printf("  -dfxor                       - decode 5/6 DADDR as XOR vs. previous data address\n");
  printf("                                 (auto-enabled by TCODE 58 ENAB.21 or a decoded 13/14)\n");
  printf("  -src N                       - N-bit SRC field after TCODE (0=none, 1-6)\n");
  printf("  -srcfilter N                 - decode only messages from source N\n");
  printf("  -srcval N                    - SRC value to encode (default 0)\n");
  printf("  -target N -pcinfo <i> -pcout <o> [-bp]\n");
  printf("                              - multi-target decode: one context per SRC value\n");
  printf("                                (repeatable; needs -src; replaces positional -pcinfo/-pcout)\n");
  printf("  -stat|-full|-all|-msg|-none - verbose level\n");
  printf("\n");
  printf("Advanced:\n");
  printf("  CTXP_TEXT_TRACEFILE=trace.ctxp.txt Cttd -deco <nex> -pcinfo <info> -pcout <pco> -none - decode trace to CTXP text format (.ctxp.txt)\n");
  printf("  CTXP_TRACEFILE=trace.ctxp Cttd -deco <nex> -pcinfo <info> -pcout <pco> -none - decode trace to CTXP binary format (.ctxp)\n");
  printf("  # (You can enable multiple exports simultaneously by setting multiple env vars)\n");
  return 1;
}

int main(int argc, char *argv[])
{
  if (InitRuntimeConfig() != 0) return 9;
  if (argc < 2) return usage(NULL);

#if WITH_EXT
  if (strcmp(argv[1], "-ext") == 0)
  {
    // Extra processing (if enabled)
    int ret = ExtProcess(argc, argv);
    return ret;
  }
#endif

  if (strcmp(argv[1], "-dump") == 0) // Dump?
  {
    if (argc < 3) return usage("Nexus bin file is expected");

    fNex = fopen(argv[2], "rb");
    if (fNex == NULL) return error("Cannot open NEX file");

    int opt = 3;
    FILE *fDump = stdout;
    if (argc > 3 && argv[3][0] != '-')
    {
      fDump = fopen(argv[3], "wt");
      if (fDump == NULL) return error("Cannot create DUMP file");
      opt = 4;
    }

    int disp = 4 | 2 | 1; // Default (all)
    for (int i = opt; i < argc; i++)
    {
      if (strcmp(argv[i], "-msg") == 0)         disp = 4 | 2;
      else if (strcmp(argv[i], "-none") == 0)   disp = 4;
      else if (strcmp(argv[i], "-src") == 0 && i + 1 < argc) { cttd_conf_src_bits = atoi(argv[++i]); if (cttd_conf_src_bits < 0 || cttd_conf_src_bits > 6) return usage("SRC bits must be 0-6"); }
      else return usage("Unknown -dump option");
    }

    int ret = NexusDump(fDump, disp);
    fclose(fNex); fNex = NULL;
    if (fDump != stdout) fclose(fDump);

    if (ret <= 0) return error("Nexus Trace dump failed");

    return 0; // All OK
  }

  if (strcmp(argv[1], "-conv") == 0) // Convert?
  {
    // -conv -objd <objd> -pcinfo <pci>
    // -conv -pcinfo <pci> -pconly <pc> -pcseq <ps>
    const char *err = "Incorrect -conv calling";

    int ret = 0;
    if ((argc == 6 || argc == 7) && strcmp(argv[2], "-objd") == 0)
    {
      // -conv -objd <objd> -pcinfo <pci> [-sijump]
      if (argc == 7)
      {
        if (strcmp(argv[6], "-sijump") != 0) return usage("Unknown -conv option");
        conf_SijumpConv = 1; // Accemic: classify inferable jalr pairs with targets
      }
      if (strcmp(argv[4], "-pcinfo") == 0)
      {
        // Syntax correct - open all files
        err = NULL;

        FILE *objdFile = fopen(argv[3], "rt");
        if (objdFile == NULL) return error("Cannot open OBJD file");

        FILE *pciFile = fopen(argv[5], "wt");
        if (pciFile == NULL) return error("Cannot create PCINFO file");

        // Run conversion
        ret = ConvGnuObjdump(objdFile, pciFile);
        fclose(pciFile);
        fclose(objdFile);
      }
    }
    else
    if (argc == 8 && strcmp(argv[2], "-pcinfo") == 0)
    {
      // -conv -pcinfo <pci> -pconly <pc> -pcseq <ps>
      if (strcmp(argv[4], "-pconly") == 0 && strcmp(argv[6], "-pcseq") == 0)
      {
        // Syntax correct - open all files
        err = NULL;

        if (InfoInit(argv[3]) < 0) return error("Cannot open PCINFO file");

        FILE *pcFile = fopen(argv[5], "rt");
        if (pcFile == NULL) return error("Cannot open PCONLY file");

        FILE *psFile = fopen(argv[7], "wt");
        if (psFile == NULL) return error("Cannot create PCSEQ file");

        // Run conversion
        ret = ConvAddInfo(pcFile, psFile, NULL);
        fclose(pcFile);
        fclose(psFile);
        InfoTerm();
      }
    }
    else
    if (argc == 6 && strcmp(argv[2], "-rtl") == 0)
    {
      // -conv -objd <objd> -pcinfo <pci>
      if (strcmp(argv[4], "-pconly") == 0)
      {
        // Syntax correct - open all files
        err = NULL;

        FILE *rtlFile = fopen(argv[3], "rt");
        if (rtlFile == NULL) return error("Cannot open RTL file");

        FILE *pcoFile = fopen(argv[5], "wt");
        if (pcoFile == NULL) return error("Cannot create PCONLY file");

        // Run conversion
        ret = ConvRtlTrace(rtlFile, pcoFile);
        fclose(pcoFile);
        fclose(rtlFile);
      }
    }

    if (err != NULL) return usage(err);

    if (ret > 0)
    {
      printf("Converted OK (%d instructions)\n\n", ret);
      ret = 0;
    }
    else
    {
      printf("ERROR: Conversion failed with error code #%d\n\n", -ret);
      ret = 9;
    }

    return ret;
  }

  if (strcmp(argv[1], "-diff") == 0) // Diff?
  {
    // -comp -pcseq <pcs> -pcout <pco>
    const char *err = "Incorrect -diff calling";

    int ret = 0;
    if (argc == 6 && (strcmp(argv[2], "-pcseq") == 0 || strcmp(argv[2], "-pconly") == 0))
    {
      // -diff -pcseq <pcseq> -pcout <pco>
      if (strcmp(argv[4], "-pcout") == 0)
      {
        // Syntax correct - open all files
        err = NULL;

        FILE *fPcseq = fopen(argv[3], "rt");
        if (fPcseq == NULL)  return error("Cannot open PCSEQ file");

        FILE *fPcout = fopen(argv[5], "rt");
        if (fPcout == NULL) return error("Cannot open PCOUT file");

        // Run comparison
        ret = ConvAddInfo(fPcseq, NULL, fPcout);

        fclose(fPcseq);
        fclose(fPcout);
      }
    }
    if (err != NULL) return usage(err);

    if (ret > 0)
    {
      printf("Compared OK (%d instructions)\n\n", ret);
      ret = 0;
    }
    else
    {
      printf("ERROR: Conversion failed with error code #%d\n\n", -ret);
      ret = 9;
    }

    return ret;
  }

  if (strcmp(argv[1], "-enco") == 0) // Encode?
  {
    if (argc < 5) return error("Incorrect number of parameters");

    if (strcmp(argv[3], "-nex") != 0) return error("-nex must be provided");

    FILE *fPcseq = fopen(argv[2], "rt");
    if (fPcseq == NULL)  return error("Cannot open PCSEQ file");

    fNex = fopen(argv[4], "wb");
    if (fNex == NULL) return error("Cannot create NEX file");

    int level = -1;     // Default level
    conf_CallStack = 0; // No callstack by default
    conf_Repeat    = 0; // No repeat detection by default
    int disp = 4;       // Default (stat)

    for (int ai = 5; ai < argc; ai++)
    {
      if (strcmp(argv[ai], "-nobhm") == 0)      level = 10;       // Level 1.0
      else if (strcmp(argv[ai], "-norbm") == 0) level = 20;       // Level 2.0
      else if (strcmp(argv[ai], "-cs") == 0)
      {
        int v;
        if (ai + 1 < argc && sscanf(argv[ai + 1], "%d", &v) == 1) { ai++; conf_CallStack = v; }
        else                                                       conf_CallStack = 8;
        if (abs(conf_CallStack) > CALLSTACK_MAX) return error("Value of -cs is too big");
      }
      else if (strcmp(argv[ai], "-rpt") == 0)
      {
        int v;
        if (ai + 1 < argc && sscanf(argv[ai + 1], "%d", &v) == 1) { ai++; conf_Repeat = v; }
        else                                                       conf_Repeat = 2;
      }
      else if (strcmp(argv[ai], "-src") == 0 && ai + 1 < argc)
      {
        cttd_conf_src_bits = atoi(argv[++ai]);
        if (cttd_conf_src_bits < 0 || cttd_conf_src_bits > 6) return usage("SRC bits must be 0-6");
      }
      else if (strcmp(argv[ai], "-srcval") == 0 && ai + 1 < argc) cttd_conf_src_value = atoi(argv[++ai]);
      else if (strcmp(argv[ai], "-all") == 0)   disp = 4 | 2 | 1;
      else if (strcmp(argv[ai], "-msg") == 0)   disp = 4 | 2;
      else if (strcmp(argv[ai], "-stat") == 0)  disp = 4;
      else if (strcmp(argv[ai], "-none") == 0)  disp = 0;
      else if (strcmp(argv[ai], "-full") == 0)  disp = 0xFF;
      else return usage("Unknown -enco option");
    }

    if (level < 0) level = 21;  // Level 2.1 is default
    int ret = NexusEnco(fPcseq, level, disp);
    fclose(fNex); fNex = NULL;
    fclose(fPcseq); fPcseq = NULL;

    if (ret > 0)
    {
      printf("Encoded OK (%d messages)\n\n", ret);
      ret = 0;
    }
    else
    {
      printf("ERROR: Encoding failed with error code #%d\n\n", -ret);
      ret = 9;
    }

    return ret;
  }

  if (strcmp(argv[1], "-deco") == 0) // Decode?
  {
    // Default = the deepest mirror the ring can hold. It must be at least as
    // deep as the ENCODER's return-address stack (CTTE: CT_RET_STACK_DEPTH
    // = 16, announced in the config message as PARAM3[12:8]); a SHALLOWER
    // mirror silently mispredicts implicit returns, a deeper one is safe. The
    // depth is overridable with -cs (W4) -- 0 switches the mirror off entirely,
    // which is only valid for streams WITHOUT implicit-return compression.
    conf_CallStack = CALLSTACK_MAX; // Always full call-stack

    if (argc < 3) return error("Incorrect number of parameters");

    // Two syntaxes:
    //   Legacy (single target, unchanged behavior):
    //     -deco <nex> -pcinfo <info> -pcout <pco> [options]
    //   Multi-target (funnel-merged stream, one context per SRC value):
    //     -deco <nex> -target <src> -pcinfo <info> -pcout <pco>
    //                [-bp] [-target <src> ...] -src <N> [options]
    const char *legacyInfo = NULL;
    const char *legacyOut = NULL;
    int optStart = 3;
    if (argc > 4 && strcmp(argv[3], "-pcinfo") == 0)
    {
      if (argc < 7) return error("Incorrect number of parameters");
      if (strcmp(argv[5], "-pcout") != 0) return error("-pcout must be provided");
      legacyInfo = argv[4];
      legacyOut = argv[6];
      optStart = 7;
    }

    fNex = fopen(argv[2], "rb");
    if (fNex == NULL) return error("Cannot open NEX file");

    int disp = 4; // Default (-stat)
    int scope = -1; // Current -target scope (multi-target syntax)
    for (int i = optStart; i < argc; i++)
    {
      if (strcmp(argv[i], "-all") == 0)         disp = 4 | 2 | 1;
      else if (strcmp(argv[i], "-msg") == 0)    disp = 4 | 2;
      else if (strcmp(argv[i], "-stat") == 0)   disp = 4;
      else if (strcmp(argv[i], "-none") == 0)   disp = 0;
      else if (strcmp(argv[i], "-full") == 0)   disp = 0xFF;
      else if (strcmp(argv[i], "-src") == 0 && i + 1 < argc) { cttd_conf_src_bits = atoi(argv[++i]); cttd_conf_src_bits_cli = 1; if (cttd_conf_src_bits < 0 || cttd_conf_src_bits > 6) return usage("SRC bits must be 0-6"); }
      else if (strcmp(argv[i], "-srcval") == 0 && i + 1 < argc) { cttd_conf_src_value = atoi(argv[++i]); }
      else if (strcmp(argv[i], "-srcfilter") == 0 && i + 1 < argc) { cttd_conf_src_filter = atoi(argv[++i]); }
      else if (strcmp(argv[i], "-target") == 0 && i + 1 < argc)
      {
        if (legacyInfo != NULL) return usage("-target cannot be combined with the positional -pcinfo/-pcout syntax");
        int t = atoi(argv[++i]);
        if (t < 0 || t >= CTTD_TARGET_MAX) return usage("-target source id out of range (0-63)");
        if (cttd_targets[t].used) return usage("-target given twice for the same source");
        cttd_targets[t].used = 1;
        cttd_target_mode = 1;
        scope = t;
      }
      else if (strcmp(argv[i], "-pcinfo") == 0 && i + 1 < argc)
      {
        if (scope < 0) return usage("-pcinfo outside a -target scope");
        cttd_targets[scope].pcinfoPath = argv[++i];
      }
      else if (strcmp(argv[i], "-pcout") == 0 && i + 1 < argc)
      {
        if (scope < 0) return usage("-pcout outside a -target scope");
        cttd_targets[scope].pcoutPath = argv[++i];
      }
      else if (strcmp(argv[i], "-bp") == 0) // Accemic VendorBP model
      {
        if (scope >= 0) cttd_targets[scope].bp = 1; // Per-target
        else { conf_BranchPredict = 1; conf_BranchPredictCli = 1; } // Global
      }
      else if (strcmp(argv[i], "-cs") == 0) // W4: return-address mirror depth
      {
        // Unlike -enco the value is MANDATORY here: the decode default is the
        // full ring, so a bare "-cs" silently shrinking the mirror to 8 would
        // be a trap (an implicit return from depth 9 would then decode to a
        // wrong PC without any message).
        int v;
        if (i + 1 >= argc || sscanf(argv[i + 1], "%d", &v) != 1)
          return usage("-cs needs a depth (0 = no return-address mirror)");
        i++;
        if (v < 0) return usage("-cs must be >= 0 for -deco (the decoder mirror has no counter-only mode)");
        if (v > CALLSTACK_MAX) return usage("Value of -cs is too big");
        conf_CallStack = v;
      }
      else if (strcmp(argv[i], "-csstrict") == 0) // W4: mismatch is fatal again
      {
        conf_CallStackStrict = 1;
      }
      else if (strcmp(argv[i], "-dfxor") == 0) // P3 DF address XOR (13/14)
      {
        // Global (applies to every target): NexusDecoStateInit seeds each
        // target's confDfXor from conf_DfXorCli, mirroring the global -bp.
        conf_DfXor = 1; conf_DfXorCli = 1;
      }
      else return usage("Unknown -deco option");
    }

    FILE *fOut = NULL;
    if (cttd_target_mode)
    {
      int nTargets = 0;
      for (int t = 0; t < CTTD_TARGET_MAX; t++)
      {
        if (!cttd_targets[t].used) continue;
        nTargets++;
        if (cttd_targets[t].pcinfoPath == NULL) return error("each -target needs a -pcinfo");
        if (cttd_targets[t].pcoutPath == NULL) return error("each -target needs a -pcout");
        if (InfoInit(cttd_targets[t].pcinfoPath) < 0) return error("Cannot open PCINFO file");
        InfoDetach(&cttd_targets[t].info);
        cttd_targets[t].fOut = fopen(cttd_targets[t].pcoutPath, "wt");
        if (cttd_targets[t].fOut == NULL) return error("Cannot create PCOUT file");
        // A global -bp (before any -target) applies to every target.
        NexusDecoStateInit(&cttd_targets[t].deco, cttd_targets[t].bp || conf_BranchPredictCli);
        CallStackStateInit(&cttd_targets[t].cs);
        if (t != 0 && cttd_conf_src_bits == 0)
        {
          printf("WARNING: -target %d configured but -src is 0 -- every message decodes as SRC 0\n", t);
        }
      }
      if (nTargets == 0) return error("no -target configured");
    }
    else
    {
      if (legacyInfo == NULL) return error("-pcinfo must be provided");
      if (InfoInit(legacyInfo) < 0) return error("Cannot open PCINFO file");
      fOut = fopen(legacyOut, "wt");
      if (fOut == NULL) return error("Cannot create PCOUT file");
    }

    int ret = NexusDeco(fOut, disp);

    if (cttd_target_mode)
    {
      for (int t = 0; t < CTTD_TARGET_MAX; t++)
      {
        if (!cttd_targets[t].used) continue;
        if (cttd_targets[t].fOut != NULL) fclose(cttd_targets[t].fOut);
        InfoStateFree(&cttd_targets[t].info);
      }
    }
    else
    {
      fclose(fOut);
      InfoTerm();
    }
    fclose(fNex); fNex = NULL;

    if (ret > 0)
    {
      printf("Decoded OK (%d instructions)\n\n", ret);
      ret = 0;
    }
    else
    {
      printf("ERROR: Decoding failed with error code #%d\n\n", -ret);
      ret = 9;
    }

    return ret;
  }

  if (strcmp(argv[1], "-decoe") == 0) // Decode E-Trace (te_inst)?
  {
    conf_CallStack = CALLSTACK_MAX; // Full call-stack (implicit-return mirror)

    if (argc < 7) return error("Incorrect number of parameters");
    if (strcmp(argv[3], "-pcinfo") != 0) return error("-pcinfo must be provided");
    if (strcmp(argv[5], "-pcout") != 0) return error("-pcout must be provided");

    fNex = fopen(argv[2], "rb");
    if (fNex == NULL) return error("Cannot open te_inst_raw file");
    if (InfoInit(argv[4]) < 0) return error("Cannot open PCINFO file");
    FILE *fOut = fopen(argv[6], "wt");
    if (fOut == NULL) return error("Cannot create PCOUT file");

    int disp = 4; // Default (-stat)
    for (int i = 7; i < argc; i++)
    {
      if (strcmp(argv[i], "-all") == 0)         disp = 4 | 2 | 1;
      else if (strcmp(argv[i], "-msg") == 0)    disp = 4 | 2;
      else if (strcmp(argv[i], "-stat") == 0)   disp = 4;
      else if (strcmp(argv[i], "-none") == 0)   disp = 0;
      else if (strcmp(argv[i], "-full") == 0)   disp = 0xFF;
      else return usage("Unknown -decoe option");
    }

    int ret = EtraceDeco(fOut, disp);
    fclose(fOut);
    fclose(fNex); fNex = NULL;
    InfoTerm();

    if (ret > 0)
    {
      printf("Decoded OK (%d instructions)\n\n", ret);
      return 0;
    }
    printf("ERROR: E-Trace decoding failed with error code #%d\n\n", -ret);
    return 9;
  }

  return usage("Unkown option");
}

//****************************************************************************
// End of cttd.c file
