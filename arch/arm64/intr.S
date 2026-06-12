/*
 * Copyright (C) 2026 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <asm/trap.h>
#include <asm/cpu.h>

/*
 * ARM64 exception entry saves a full cpu_regs frame on the current EL2 stack.
 * Host exceptions return through dispatch_trap, while guest exceptions enter
 * vcpu_exit.c and then branch to vcpu_exit_return so the modified guest frame
 * is restored directly into EL1 with ERET.
 */
.macro save_cpu_context
	stp	x0, x1, [sp, #CPU_REGS_OFFSET_X0]
	stp	x2, x3, [sp, #CPU_REGS_OFFSET_X2]
	stp	x4, x5, [sp, #CPU_REGS_OFFSET_X4]
	stp	x6, x7, [sp, #CPU_REGS_OFFSET_X6]
	stp	x8, x9, [sp, #CPU_REGS_OFFSET_X8]
	stp	x10, x11, [sp, #CPU_REGS_OFFSET_X10]
	stp	x12, x13, [sp, #CPU_REGS_OFFSET_X12]
	stp	x14, x15, [sp, #CPU_REGS_OFFSET_X14]
	stp	x16, x17, [sp, #CPU_REGS_OFFSET_X16]
	stp	x18, x19, [sp, #CPU_REGS_OFFSET_X18]
	stp	x20, x21, [sp, #CPU_REGS_OFFSET_X20]
	stp	x22, x23, [sp, #CPU_REGS_OFFSET_X22]
	stp	x24, x25, [sp, #CPU_REGS_OFFSET_X24]
	stp	x26, x27, [sp, #CPU_REGS_OFFSET_X26]
	stp	x28, x29, [sp, #CPU_REGS_OFFSET_X28]
	str	x30, [sp, #CPU_REGS_OFFSET_LR]
	mrs	x0, elr_el2
	str	x0, [sp, #CPU_REGS_OFFSET_ELR]
	mrs	x0, spsr_el2
	str	x0, [sp, #CPU_REGS_OFFSET_SPSR]
	mrs	x0, esr_el2
	str	x0, [sp, #CPU_REGS_OFFSET_ESR]
	mrs	x0, far_el2
	str	x0, [sp, #CPU_REGS_OFFSET_FAR]
	mrs	x0, hpfar_el2
	str	x0, [sp, #CPU_REGS_OFFSET_HPFAR]
	add	x0, sp, #CPU_REGS_OFFSET_LAST
	str	x0, [sp, #CPU_REGS_OFFSET_SP]
	str	x0, [sp, #CPU_REGS_OFFSET_EXC_SP]
.endm

.macro restore_cpu_context
	ldr	x0, [sp, #CPU_REGS_OFFSET_ELR]
	msr	elr_el2, x0
	ldr	x0, [sp, #CPU_REGS_OFFSET_SPSR]
	msr	spsr_el2, x0
	ldp	x0, x1, [sp, #CPU_REGS_OFFSET_X0]
	ldp	x2, x3, [sp, #CPU_REGS_OFFSET_X2]
	ldp	x4, x5, [sp, #CPU_REGS_OFFSET_X4]
	ldp	x6, x7, [sp, #CPU_REGS_OFFSET_X6]
	ldp	x8, x9, [sp, #CPU_REGS_OFFSET_X8]
	ldp	x10, x11, [sp, #CPU_REGS_OFFSET_X10]
	ldp	x12, x13, [sp, #CPU_REGS_OFFSET_X12]
	ldp	x14, x15, [sp, #CPU_REGS_OFFSET_X14]
	ldp	x16, x17, [sp, #CPU_REGS_OFFSET_X16]
	ldp	x18, x19, [sp, #CPU_REGS_OFFSET_X18]
	ldp	x20, x21, [sp, #CPU_REGS_OFFSET_X20]
	ldp	x22, x23, [sp, #CPU_REGS_OFFSET_X22]
	ldp	x24, x25, [sp, #CPU_REGS_OFFSET_X24]
	ldp	x26, x27, [sp, #CPU_REGS_OFFSET_X26]
	ldp	x28, x29, [sp, #CPU_REGS_OFFSET_X28]
	ldr	x30, [sp, #CPU_REGS_OFFSET_LR]
.endm

.macro vector_entry type
	sub	sp, sp, #CPU_REGS_OFFSET_LAST
	save_cpu_context
	mov	x0, sp
	mov	x1, #\type
	bl	dispatch_trap
	restore_cpu_context
	add	sp, sp, #CPU_REGS_OFFSET_LAST
	eret
.endm

	.text
	.align 11
	.globl arm64_exception_vectors
arm64_exception_vectors:
	b	arm64_sync_handler
	.balign 0x80
	b	arm64_irq_handler
	.balign 0x80
	b	arm64_fiq_handler
	.balign 0x80
	b	arm64_serror_handler

	.balign 0x80
	b	arm64_sync_handler
	.balign 0x80
	b	arm64_irq_handler
	.balign 0x80
	b	arm64_fiq_handler
	.balign 0x80
	b	arm64_serror_handler

	.balign 0x80
	b	arm64_guest_sync_handler
	.balign 0x80
	b	arm64_guest_irq_handler
	.balign 0x80
	b	arm64_guest_fiq_handler
	.balign 0x80
	b	arm64_guest_serror_handler

	.balign 0x80
	b	arm64_sync_handler
	.balign 0x80
	b	arm64_irq_handler
	.balign 0x80
	b	arm64_fiq_handler
	.balign 0x80
	b	arm64_serror_handler

	.balign 0x80
arm64_sync_handler:
	vector_entry ARM64_TRAP_SYNC

arm64_irq_handler:
	vector_entry ARM64_TRAP_IRQ

arm64_fiq_handler:
	vector_entry ARM64_TRAP_FIQ

arm64_serror_handler:
	vector_entry ARM64_TRAP_SERROR

	.macro guest_vector_entry handler
	/* Preserve the guest's SP_EL1 separately from the EL2 vector stack. */
	sub	sp, sp, #CPU_REGS_OFFSET_LAST
	save_cpu_context
	mrs	x0, sp_el1
	str	x0, [sp, #CPU_REGS_OFFSET_SP]
	mov	x0, sp
	bl	\handler
	b	vcpu_exit_return
	.endm

arm64_guest_sync_handler:
	guest_vector_entry dispatch_vcpu_trap

arm64_guest_irq_handler:
	guest_vector_entry dispatch_vcpu_irq

arm64_guest_fiq_handler:
	guest_vector_entry dispatch_vcpu_trap

arm64_guest_serror_handler:
	guest_vector_entry dispatch_vcpu_trap

	.globl vcpu_exit_return
vcpu_exit_return:
	/* Restore guest architectural state from the frame prepared by C code. */
	ldr	x0, [sp, #CPU_REGS_OFFSET_ELR]
	msr	elr_el2, x0
	ldr	x0, [sp, #CPU_REGS_OFFSET_SPSR]
	msr	spsr_el2, x0
	ldr	x0, [sp, #CPU_REGS_OFFSET_SP]
	msr	sp_el1, x0
	ldp	x0, x1, [sp, #CPU_REGS_OFFSET_X0]
	ldp	x2, x3, [sp, #CPU_REGS_OFFSET_X2]
	ldp	x4, x5, [sp, #CPU_REGS_OFFSET_X4]
	ldp	x6, x7, [sp, #CPU_REGS_OFFSET_X6]
	ldp	x8, x9, [sp, #CPU_REGS_OFFSET_X8]
	ldp	x10, x11, [sp, #CPU_REGS_OFFSET_X10]
	ldp	x12, x13, [sp, #CPU_REGS_OFFSET_X12]
	ldp	x14, x15, [sp, #CPU_REGS_OFFSET_X14]
	ldp	x16, x17, [sp, #CPU_REGS_OFFSET_X16]
	ldp	x18, x19, [sp, #CPU_REGS_OFFSET_X18]
	ldp	x20, x21, [sp, #CPU_REGS_OFFSET_X20]
	ldp	x22, x23, [sp, #CPU_REGS_OFFSET_X22]
	ldp	x24, x25, [sp, #CPU_REGS_OFFSET_X24]
	ldp	x26, x27, [sp, #CPU_REGS_OFFSET_X26]
	ldp	x28, x29, [sp, #CPU_REGS_OFFSET_X28]
	ldr	x30, [sp, #CPU_REGS_OFFSET_LR]
	add	sp, sp, #CPU_REGS_OFFSET_LAST
	eret
