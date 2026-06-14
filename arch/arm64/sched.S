/*
 * Copyright (C) 2026 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <asm/cpu.h>

	.text
	.align 4
	.globl arch_switch_to
arch_switch_to:
	sub	sp, sp, #STACK_FRAME_SIZE

	/* Save callee-saved GPRs; caller-saved registers belong to the caller. */
	stp	x19, x20, [sp, #STACK_FRAME_OFFSET_X19]
	stp	x21, x22, [sp, #STACK_FRAME_OFFSET_X21]
	stp	x23, x24, [sp, #STACK_FRAME_OFFSET_X23]
	stp	x25, x26, [sp, #STACK_FRAME_OFFSET_X25]
	stp	x27, x28, [sp, #STACK_FRAME_OFFSET_X27]
	stp	x29, x30, [sp, #STACK_FRAME_OFFSET_X29]
	str	x0, [sp, #STACK_FRAME_OFFSET_X0]
	/* DAIF is the interrupt mask state; keep it per scheduled context. */
	mrs	x2, daif
	str	x2, [sp, #STACK_FRAME_OFFSET_SPSR]

	/* Store old SP through *prev_sp, then load next SP from *next_sp. */
	mov	x2, sp
	str	x2, [x0]
	ldr	x2, [x1]
	mov	sp, x2

	/* Restore the next context's preserved registers and interrupt mask. */
	ldp	x19, x20, [sp, #STACK_FRAME_OFFSET_X19]
	ldp	x21, x22, [sp, #STACK_FRAME_OFFSET_X21]
	ldp	x23, x24, [sp, #STACK_FRAME_OFFSET_X23]
	ldp	x25, x26, [sp, #STACK_FRAME_OFFSET_X25]
	ldp	x27, x28, [sp, #STACK_FRAME_OFFSET_X27]
	ldp	x29, x30, [sp, #STACK_FRAME_OFFSET_X29]
	ldr	x0, [sp, #STACK_FRAME_OFFSET_X0]
	ldr	x2, [sp, #STACK_FRAME_OFFSET_SPSR]
	/* Restoring DAIF reestablishes whether IRQ/FIQ/debug/SError were masked. */
	msr	daif, x2
	add	sp, sp, #STACK_FRAME_SIZE

	ret
