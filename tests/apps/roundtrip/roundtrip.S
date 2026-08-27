/* SPDX-FileCopyrightText: 2026 Accemic Technologies GmbH */
/* SPDX-License-Identifier: ISC */
/*****************************************************************************
 * Tiny RISC-V program for the CTTD encode -> decode round-trip test.
 *
 * Deliberately hand-written so every control-flow edge the decoder must
 * reconstruct is exercised in a short, readable trace:
 *   - a conditional direct branch, taken (loop back-edge) and not-taken
 *   - a direct call (jal ra) and its return (jalr x0, ra)
 *   - a direct jump (jal x0)
 *
 * .option norvc keeps every instruction 4 bytes so the addresses below are
 * trivially predictable. Linked at 0x1000 (see Makefile / run_tests.sh).
 *****************************************************************************/
	.option norvc
	.section .text
	.globl  _start
_start:
	addi    t0, x0, 3          /* 0x1000  loop counter = 3            */
	addi    t1, x0, 0          /* 0x1004  accumulator = 0            */
loop:
	addi    t1, t1, 1          /* 0x1008                             */
	addi    t0, t0, -1         /* 0x100c                             */
	bne     t0, x0, loop       /* 0x1010  taken while t0 != 0 (BD4)  */
	jal     ra, func           /* 0x1014  direct call (CD4)          */
	jal     x0, done           /* 0x1018  direct jump (JD4)          */
func:
	addi    a0, t1, 0          /* 0x101c                             */
	jalr    x0, 0(ra)          /* 0x1020  return (RI)                */
done:
	jal     x0, done           /* 0x1024  self-loop = end of trace   */
