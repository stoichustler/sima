/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <asm/cpu.h>
#include <asm/trap.h>

#define VCPU_RUN_STACK_SIZE	CPU_REGS_OFFSET_LAST

	.text
	.align 4
	.globl arm64_run_vcpu
arm64_run_vcpu:
	/*
	 * 2026-06-30, vCPU entry principle:
	 *
	 * Build the same cpu_regs frame that guest_vector_entry would have saved
	 * on an exit. vcpu_exit_return then programs ELR_EL2/SPSR_EL2/SP_EL1 and
	 * executes ERET, so first entry and trap return share one restore path.
	 *
	 *   vcpu->arch.regs -> thread-stack cpu_regs -> vcpu_exit_return -> ERET
	 */
	sub	sp, sp, #VCPU_RUN_STACK_SIZE
	/* x16 is the source vCPU register frame, x17 is the temporary EL2 frame. */
	mov	x16, x0
	mov	x17, sp
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_X0]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_X0]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_X2]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_X2]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_X4]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_X4]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_X6]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_X6]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_X8]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_X8]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_X10]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_X10]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_X12]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_X12]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_X14]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_X14]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_X16]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_X16]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_X18]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_X18]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_X20]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_X20]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_X22]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_X22]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_X24]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_X24]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_X26]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_X26]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_X28]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_X28]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_LR]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_LR]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_ELR]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_ELR]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_ESR]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_ESR]
	ldp	x0, x1, [x16, #CPU_REGS_OFFSET_HPFAR]
	stp	x0, x1, [x17, #CPU_REGS_OFFSET_HPFAR]
	ldr	x0, [x16, #CPU_REGS_OFFSET_EXC_SP]
	str	x0, [x17, #CPU_REGS_OFFSET_EXC_SP]
	b	vcpu_exit_return
