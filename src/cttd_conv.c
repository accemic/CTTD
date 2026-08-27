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
// File cttd_conv.c  - Nexus RISC-V Trace converter (to aid decoding)

// Code below is written in plain C-code.
// It was compiled using VisualC, GNU and IAR C/C++ compiler.
//  1. Only standard C-types are used.
//  2. Only few standard C functions used - see notes with "#include <...>"
//  3. Only non K&R C is 'for (int x' and 'int x;' between instructions.

#include <stdio.h>  //  For NULL, 'printf', 'fopen, ...'
#include <stdlib.h> //  For 'exit'
#include <string.h> //  For 'strcmp', 'strchr' 
#include <ctype.h>  //  For 'isspace/isxdigit' etc.
#include <inttypes.h>   //  For scan formats SCNx64

#include "cttd.h"  //  Common NEXUS_... #define (RISC-V specific subset)

#include "cttd_info.h"  // We need info 

// It converts GNU-objdump file (with -d option) to info-file

// This is example line (<t> denotes TAB) - line must start from space!
// 166:<t>0ba000ef          <t>jal<t>ra, 220 <c_ecall_handler_v2>

static Nexus_TypeAddr GetParAddr(const char *l)
{
  // Skip over opcode (and '/t' following it)
  while (!(*l == '\t' || *l == '\0')) l++;
  if (*l++ != '\t') return 1;

  // First parameter or after last ','
  const char *parAddr = l;
  while (!(*l == '\t' || *l == '\0'))
  {
    if (*l == ',') parAddr = l + 1;
    l++;
  }

  if (!isxdigit(*parAddr)) return 3;

  Nexus_TypeAddr addr;
  if (sscanf(parAddr, "%" SCNx64, &addr) != 1) return 5;
  return addr;
}

int ConvRtlTrace(FILE *fIn, FILE *fOut)
{
  char line[1000];

  int nInstr = 0;
  while (fgets(line, sizeof(line), fIn) != NULL)
  {
    char *p = strstr(line, " I ");
    if (p == NULL || (p - 6) < line)
    {
      continue;
    }
    // (M     ) 0:0:0 I 000000001fc00cfc ---------------- 02835313   srli    x6, x6, 40  # CycleCount=4147     InstrCount=227    SeqNum=35  itag=00000e7
    //          -----0+++++++++11111111112
    // p-+:     54321012345678901234567890
    if (*(p - 2) != ':' || *(p - 4) != ':' || p[20] != '-') // Extra checking ...
    {
      continue;
    }
    p += 3;           // Isolate 64-bit (16-digit) hex number 
    p[16] = '\0';

    while (*p == '0') // Skip leading 0-s
      p++;
    if (*p == '\0') p--;

    fprintf(fOut, "0x%s\n", p);
    nInstr++;
  }

  return nInstr;
}

// --- rd-aware jal/jalr classification (2026-07-18, Accemic) ---------------------------------
// The MicroBlaze-V board flow disassembles with `-M no-aliases,numeric` (required so the RTL itype
// decoder sees rd). In that form jumps and returns appear as raw jal/jalr, NOT as j/ret/jr
// pseudos -- so classifying every jal as a call and every jalr as an indirect call is wrong:
//   jal  x0,<a>      is an unconditional JUMP (rd=zero, no link)     -> must NOT push a frame
//   jalr x0,<o>(x1)  is a RETURN (ret; rd=zero, base=ra)            -> pops the call stack
//   jalr x0,<o>(xN)  is an indirect JUMP (jr; rd=zero, base!=ra)    -> no stack op
// Misclassifying `jal x0` as a call left a spurious return address on the stack that a later
// `mret` popped -> "call-stack target ... != trace target" abort on programs with calls+traps.
// instr points at the mnemonic field, e.g. "jal\tx0,10 <...>" / "jalr\tx0,0(x1)".

// Destination register number (the xN right after the mnemonic), or -1 if not parseable.
static int GnuRdNum(const char *instr)
{
  const char *p = instr;
  while (*p != '\0' && *p != '\t' && *p != ' ') p++;   // skip mnemonic
  while (*p == '\t' || *p == ' ') p++;                  // skip separator(s)
  if (*p != 'x') return -1;
  p++;
  int r = 0, any = 0;
  while (*p >= '0' && *p <= '9') { r = r * 10 + (*p - '0'); p++; any = 1; }
  return any ? r : -1;
}

// For jalr "<rd>,<off>(xBASE)": the base register number inside the parentheses, or -1.
static int GnuJalrBaseNum(const char *instr)
{
  const char *p = instr;
  while (*p != '\0' && *p != '(') p++;
  if (*p != '(') return -1;
  p++;
  if (*p != 'x') return -1;
  p++;
  int r = 0, any = 0;
  while (*p >= '0' && *p <= '9') { r = r * 10 + (*p - '0'); p++; any = 1; }
  return any ? r : -1;
}

extern int conf_SijumpConv; // cttd.c (-conv ... -sijump)

int ConvGnuObjdump(FILE *fObjd, FILE *fPcInfo)
{
  char line[1000];

  // Accemic sijump (-sijump): track the previous instruction line so an
  // ADJACENT auipc/lui + jalr pair can be classified with a computed target
  // (mirrors the encoder-side adapter rule in mbv_to_ctrace_tip).
  int            sijPrevUType = 0;   // previous instr was auipc/lui with rd != x0
  unsigned int   sijPrevRd    = 0;
  Nexus_TypeAddr sijPrevVal   = 0;   // value the U-type wrote into rd
  Nexus_TypeAddr sijPrevEnd   = 0;   // addr + size of the previous instr

  int nInstr = 0;
  while (fgets(line, sizeof(line), fObjd) != NULL)
  {
    const char *l = line;
    while (isspace(*l)) l++;
    if (!isxdigit(*l))
    {
      continue;
    }

    Nexus_TypeAddr addr;
    if (sscanf(l, "%" SCNx64, &addr) != 1) return -1;
    while (isxdigit(*l)) l++;
    if (*l++ != ':') continue;
    if (*l++ != '\t') continue;

    int size = 4;
    if (l[4] == ' ') size = 2;
    unsigned int code;
    if (sscanf(l, "%x", &code) != 1) return -21;

    while (!(*l == '\t' || *l == '\0')) l++;
    if (*l++ != '\t') 
    {
      // return -22;  // Original (stop on error ...)
      continue; // Instead stopping, skip lines which we do not like ...
    }

    // Here we need to recognize the following opcodes
    //  b??/j/jal (with address)
    //  jalr/jr/ret/mret (without address)

    int disp = 0;
    const char *instr = l;

    if (instr[0] == 'j' || instr[0] == 'b' || instr[0] == 'r' || instr[0] == 'm')
    {
      // j <a> or jal <reg>,<a>
      disp = 0; // For debugging 
    }

    if (disp)
    {
      printf("addr=0x%" PRIX64 ",code=0x%X,size=%d,instr=%s\n", addr, code, size, instr);
    }

    // Determine instruction type based on opcode of instruction
    const char *iType = "L";
    if (instr[0] == 'c' && instr[1] == '.')
    {
      // Compressed control flow (RV32C, -M no-aliases,numeric): c.j/c.jal/c.jr/c.jalr/c.beqz/c.bnez.
      // Only relevant with C-extension enabled (G7); MVP is RV32 without C. c.jal/c.jalr always link
      // to ra (call); c.j does not (jump); c.jr x1 is a return, c.jr xN is an indirect jump.
      const char *m = instr + 2;
      if      (m[0] == 'j' && m[1] == 'a' && m[2] == 'l' && m[3] == 'r') iType = "CI"; // c.jalr
      else if (m[0] == 'j' && m[1] == 'a' && m[2] == 'l')                iType = "CD"; // c.jal
      else if (m[0] == 'j' && m[1] == 'r') iType = (GnuRdNum(instr) == 1) ? "R" : "JI"; // c.jr x1=ret
      else if (m[0] == 'j')                                             iType = "JD"; // c.j
      else if (m[0] == 'b' && (m[1] == 'e' || m[1] == 'n'))             iType = "BD"; // c.beqz/c.bnez
    }
    else if (instr[0] == 'j' || (instr[0] == 'b' && instr[4] != 'i'))
      // That 'i' for for bseti/bclri/bexti/binvi - see https://github.com/riscv/riscv-opcodes/blob/master/rv32_zbs
    {
      // "j <a>" or "jal <r>,<a>" or "jr <r>" or "jalr <r>"
      // "b?? ...<a>
      if (instr[0] =='j' && instr[1] == 'r')
      {
        // jr does not have an address (alias form, if aliases are enabled)
        iType = "JI"; // Jump indirect
      }
      else
      if (instr[0] == 'j' && instr[1] == 'a' && instr[3] == 'r')
      {
        // jalr <rd>,<off>(<base>): rd distinguishes call from return/indirect-jump.
        int rd = GnuRdNum(instr);
        if (rd == 0)
        {
          int base = GnuJalrBaseNum(instr);
          iType = (base == 1) ? "R" : "JI"; // jalr x0,off(x1)=ret ; jalr x0,off(xN)=jr
        }
        else
        {
          iType = "CI"; // Call indirect (linked)
        }
      }
      else
      {
        if (instr[0] == 'b')                    iType = "BD"; // Branch direct
        if (instr[0] == 'j' && instr[1] != 'a') iType = "JD"; // Jump direct (j, alias)
        if (instr[0] == 'j' && instr[1] == 'a')
        {
          // jal <rd>,<a>: rd=x0 is an unconditional jump (no link), else a call.
          iType = (GnuRdNum(instr) == 0) ? "JD" : "CD";
        }
      }
    }
    else
    {
      // System traps are stack-neutral: their target comes from the trace, not the call stack.
      //   mret/sret: trap RETURN -> returns to mepc/sepc (mepc+4 for ecall, but the preempted PC
      //              for interrupts) -- the call stack cannot predict that, so classify as an
      //              indirect JUMP (JI): use the trace target, do NOT pop/check the call stack.
      //   ecall/ebreak: trap SOURCE -> redirect to the handler (from the trace); no call-stack push.
      // (With -M no-aliases, 'ret'/'jr' appear as jalr and are classified above; the 'ret' alias
      //  branch here only matters for alias-form dumps.)
      if ((instr[0] == 'm' || instr[0] == 's') && instr[1] == 'r'
          && instr[2] == 'e' && instr[3] == 't')
        iType = "JI"; // mret / sret
      else if (instr[0] == 'r' && instr[1] == 'e' && instr[2] == 't')
        iType = "R";  // ret (alias form only)
      else if (instr[0] == 'e' && instr[1] == 'c')
        iType = "JI"; // ecall
      else if (instr[0] == 'e' && instr[1] == 'b')
        iType = "JI"; // ebreak
    }

    // Accemic sijump conversion: a jalr whose base register value is
    // statically known -- (a) rs1 written by the DIRECTLY preceding
    // auipc/lui, or (b) rs1 = x0 (constant base; linker relaxation emits
    // this for absolute low targets) -- becomes a direct call/jump WITH a
    // computed target, so the decoder can walk encoder-folded sequential
    // jumps. RETURN (rs1 = link reg) and co-routine forms keep their
    // dynamic classification, mirroring the adapter.
    int sijHaveTarget = 0;
    Nexus_TypeAddr sijTarget = 0;
    if (conf_SijumpConv && size == 4 && (code & 0x7F) == 0x67)
    {
      unsigned int rdN  = (code >> 7)  & 0x1F;
      unsigned int rs1N = (code >> 15) & 0x1F;
      int rdLink  = (rdN == 1 || rdN == 5);
      int rs1Link = (rs1N == 1 || rs1N == 5);
      int imm12 = ((int)code) >> 20;   // sign-extended I-immediate
      Nexus_TypeAddr base = 0;
      int baseKnown = 0;
      if (rs1N == 0) { base = 0; baseKnown = 1; }
      else if (sijPrevUType && sijPrevRd == rs1N && sijPrevEnd == addr)
      { base = sijPrevVal; baseKnown = 1; }
      if (baseKnown)
      {
        if      (rdLink && !(rs1Link && rs1N != rdN)) iType = "CD"; // uninferable call -> inferable
        else if (!rdLink && !rs1Link)                 iType = "JD"; // uninferable jump -> inferable
        else                                          baseKnown = 0; // return / co-routine: dynamic
      }
      if (baseKnown)
      {
        sijTarget = (base + (Nexus_TypeAddr)(long long)imm12)
                    & ~(Nexus_TypeAddr)1 & 0xFFFFFFFFu;
        sijHaveTarget = 1;
      }
    }

    // Produce output record
    fprintf(fPcInfo, "0x%" PRIX64 ",%s%d", addr, iType, size);

    if (sijHaveTarget)
    {
      fprintf(fPcInfo, ",0x%" PRIX64, sijTarget);
    }
    else if (iType[1] == 'D')
    {
      // Direct (branch/call/jump) - we need to extract destination address
      // from parameters. Address may be first or after last ','
      Nexus_TypeAddr destAddr = GetParAddr(instr);
      if (destAddr & 1) return -(30 + (int)destAddr);
      fprintf(fPcInfo, ",0x%" PRIX64, destAddr);
    }
    fprintf(fPcInfo, "\n");

    // Track for the next line's pair rule (only 32-bit U-types qualify).
    sijPrevUType = (size == 4)
                   && (((code & 0x7F) == 0x17) || ((code & 0x7F) == 0x37))
                   && (((code >> 7) & 0x1F) != 0);
    sijPrevRd    = (code >> 7) & 0x1F;
    sijPrevVal   = (((code & 0x7F) == 0x17)
                    ? (addr + (Nexus_TypeAddr)(int)(code & 0xFFFFF000))
                    : (Nexus_TypeAddr)(int)(code & 0xFFFFF000)) & 0xFFFFFFFFu;
    sijPrevEnd   = addr + (Nexus_TypeAddr)size;

    nInstr++;
  }

  return nInstr;
}

int ConvAddInfo(FILE *fIn, FILE *fOut, FILE *fComp)
{
  // Scan PC-sequence file and add INFO for each PC
  char line[1000];

  Nexus_TypeAddr branchAddr = 0;  // Address of branch instruction
  unsigned int branchSize = 0;  // Size of previous branch

  int nInstr = 0;
  while (fgets(line, sizeof(line), fIn) != NULL)
  {
    const char *l = line;
    while (isspace(*l)) l++;

    if (0) printf("%s", l); // For debugging ...

#if 0
    // It must be PC in format '0xHHH'
    if (l[0] != '0' || !(l[1] == 'x' || l[1] == 'X'))
    {
      fprintf(fOut, "ERROR: Line %s does not have PC with 0x prefix\n", line);
      return -1;
    }
#endif

    if (l[0] == '.')
    {
      // Handle end-marker ...
      if (l[1] == 'e')
      {
        break;
      }
    }

    Nexus_TypeAddr a;
    if (sscanf(l, "%" SCNx64, &a) != 1)
    {
      fprintf(fOut, "ERROR: Line %s does not have PC with 0x prefix\n", line);
      return -2;
    }

    if (0) printf("ADDR=0x%" PRIX64 "\n", a); // For debugging ...

    if (fOut == NULL)
    {
      nInstr++;
      if (fComp != NULL)
      {
        if (fgets(line, sizeof(line), fComp) == NULL)
        {
          printf("ERROR: Instruction #%d at address 0x%" PRIX64 " - no PC at PCOUT file.\n", nInstr, a);
          return -5;
        }
        const char *l = line;

        Nexus_TypeAddr aa;
        if (sscanf(l, "%" SCNx64, &aa) != 1)
        {
          printf("ERROR: Line %s does not have PC with 0x prefix\n", line);
          return -6;
        }

        if (a != aa)
        {
          printf("ERROR: Instruction #%d mismatch. Expected 0x%" PRIX64 ", actual 0x%" PRIX64 "\n", nInstr, a, aa);
          return -4;
        }
      }

      continue; // Done (no need for INFO processing)
    }

    InfoAddr aa;
    unsigned int info = InfoGet(a, &aa);
    if (info == 0)
    {
#if 1 // This is needed for processing of files generated from Spike (there are 5 instructions at the beginning ...)
      if (nInstr == 0) continue;  // Skip initial wrong addresses ...
#endif      
      fprintf(fOut, "ERROR: Instruction at address 0x%" PRIX64 " not found in <info-file>\n", a);
      return -3;
    }

    if (branchSize != 0)
    {
      // Previous instruction was branch - let's see if branch was taken or not
      if (branchAddr + branchSize == a)
      {
        // Branch was not taken
        fprintf(fOut, "N");
      }
      fprintf(fOut, "%d\n", branchSize);
      branchSize = 0; // One time deal
    }

    nInstr++;

    fprintf(fOut, "0x%" PRIX64, a); // Output PC value
    // Append type of instruction to plain PC value
    if (info & INFO_CALL)         fprintf(fOut, ",C");
    else if (info & INFO_RET)     fprintf(fOut, ",R");
    else if (info & INFO_JUMP)    fprintf(fOut, ",J");
    else if (info & INFO_BRANCH)  fprintf(fOut, ",B");
    else                          fprintf(fOut, ",L");

    // Report non-taken branch as BN

    if (info & (INFO_INDIRECT) && !(info & INFO_RET)) fprintf(fOut, "I");

    if (info & INFO_BRANCH)
    {
      // Special handling of branch - we must know next address
      branchAddr = a;
      if (info & INFO_4) branchSize = 4; else branchSize = 2;
      continue;  // B<s> or BN<s> will be displayed in next loop
    }

    if (info & INFO_4) fprintf(fOut, "4"); else fprintf(fOut, "2");

#if 0 // No need to report direct address
    if (info & (INFO_BRANCH | INFO_JUMP | INFO_CALL))
    {
      if (!(info & INFO_INDIRECT))
      {
        fprintf(fOut, ",0x%" PRIX64, aa); // Direct address
      }
    }
#endif

    fprintf(fOut, "\n");
  }

  return nInstr;
}

static int ConvBin4(FILE *fIn, FILE *fOut)
{
  // Flip nibbles (in-place) - it may compress buffer as well, what will speed-up processing

  //  FF  FF
  //  1F  21
  //  72  47
  //  F4  FF
  //  BA  BA 
  //  C7  C7

  //  FF  FF
  //  1F  21
  //  72  47
  //  F4  FF
  //  AF  AB 
  //  CD  CD

  //  FF 
  //  FF
  //  ab
  //  c7
  //  1F
  //  
  int status = 0;

  unsigned char b;
  unsigned char nb;
  int nOk = 0;

  int nWr = 0;
  while (nOk || fread(&b, 1, 1, fIn) == 1)
  {
    if (nOk) { b = nb; nOk = 0; } // Get next (if given)

    if (status == 0)  // We had idles recently
    {
      if (b == 0xFF)
      {
        continue;     // Skip many idle bytes
      }
      if ((b & 0xF) == 0xF)
      {
        status = 1; // This will flip as we have '1F' now
      }
      else
      {
        status = 2; // This is normal-case byte
      }
    }

    if (status == 1)  // Flip
    {
      nOk = 1;
      if (fread(&nb, 1, 1, fIn) != 1) return -1;

      //    MSB                LSB
      b = ((nb & 0xF) << 4) | (b >> 4);
      if ((b & 0x3) == 0x3 && (nb >> 4) == 0xF)
      {
        nb = 0xFF;
        status = 0; // Idle start
      }
    }
    else
    {
      // This is normal byte (not flipped)
      if ((b & 3) == 0x3)
      {
        nOk = 1;
        if (fread(&nb, 1, 1, fIn) != 1) return -1;

        if (nb == 0xFF)
        {
          status = 0;
        }
        else if ((nb & 0xF) == 0xF)
        {
          status = 1;
        }
      }
    }

    if (fwrite(&b, 1, 1, fOut) != 1) return -2;
    nWr++;
  }

  return nWr;
}

//****************************************************************************
// End of cttd_conv.c file
