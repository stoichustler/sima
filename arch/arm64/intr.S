/*
 * Copyright (C) 2026 Hustler Lo.
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
	/* ELR_EL2 is the PC that ERET will resume after the exception. */
	mrs	x0, elr_el2
	str	x0, [sp, #CPU_REGS_OFFSET_ELR]
	/* SPSR_EL2 stores the pre-exception PSTATE, including DAIF and mode. */
	mrs	x0, spsr_el2
	str	x0, [sp, #CPU_REGS_OFFSET_SPSR]
	/* ESR_EL2 describes the exception class and instruction syndrome. */
	mrs	x0, esr_el2
	str	x0, [sp, #CPU_REGS_OFFSET_ESR]
	/* FAR_EL2 is the faulting virtual address for data/instruction aborts. */
	mrs	x0, far_el2
	str	x0, [sp, #CPU_REGS_OFFSET_FAR]
	/* HPFAR_EL2 carries the stage-2 IPA fault address for guest aborts. */
	mrs	x0, hpfar_el2
	str	x0, [sp, #CPU_REGS_OFFSET_HPFAR]
	add	x0, sp, #CPU_REGS_OFFSET_LAST
	str	x0, [sp, #CPU_REGS_OFFSET_SP]
	str	x0, [sp, #CPU_REGS_OFFSET_EXC_SP]
.endm

.macro restore_cpu_context
	ldr	x0, [sp, #CPU_REGS_OFFSET_ELR]
	/* Restore the EL2 return PC consumed by ERET. */
	msr	elr_el2, x0
	ldr	x0, [sp, #CPU_REGS_OFFSET_SPSR]
	/* Restore PSTATE so ERET returns with the original masks and EL mode. */
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
	/* Current EL with SP0: synchronous, IRQ, FIQ, SError. */
	b	arm64_sync_handler
	.balign 0x80
	b	arm64_irq_handler
	.balign 0x80
	b	arm64_fiq_handler
	.balign 0x80
	b	arm64_serror_handler

	.balign 0x80
	/* Current EL with SPx: normal EL2 host exceptions use these slots. */
	b	arm64_sync_handler
	.balign 0x80
	b	arm64_irq_handler
	.balign 0x80
	b	arm64_fiq_handler
	.balign 0x80
	b	arm64_serror_handler

	.balign 0x80
	/* Lower EL AArch64: guest EL1 traps and interrupts land here. */
	b	arm64_guest_sync_handler
	.balign 0x80
	b	arm64_guest_irq_handler
	.balign 0x80
	b	arm64_guest_fiq_handler
	.balign 0x80
	b	arm64_guest_serror_handler

	.balign 0x80
	/* Lower EL AArch32 is not supported; route it to the host trap path. */
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
	/* SP_EL1 is the guest kernel stack pointer when EL1 uses SP_EL1. */
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
	/*
	 * Keep current-EL IRQs out of the ELR_EL2/SPSR_EL2 restore window.
	 * A host IRQ taken after ELR_EL2 is programmed but before ERET would use
	 * ELR_EL2 for its own EL2 return address, corrupting the guest PC.
	 */
	msr	daifset, #2
	/* Restore guest architectural state from the frame prepared by C code. */
	ldr	x0, [sp, #CPU_REGS_OFFSET_ELR]
	/* ELR_EL2 holds the guest PC used by ERET back to EL1. */
	msr	elr_el2, x0
	ldr	x0, [sp, #CPU_REGS_OFFSET_SPSR]
	/* SPSR_EL2 selects guest EL1h/EL1t and restores guest PSTATE. */
	msr	spsr_el2, x0
	ldr	x0, [sp, #CPU_REGS_OFFSET_SP]
	/* Restore the guest's SP_EL1 before returning to EL1. */
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
