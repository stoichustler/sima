/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <asm/cpu.h>

	.section .text.entry, "ax", %progbits

	/*
	 * Main entry point.
	 * x0 = firmware-provided CPU identifier or scratch value
	 * x1 = DTB physical address
	 */
	.globl _start
_start:
	/* DAIF masks Debug, SError, IRQ, and FIQ while EL2 state is incomplete. */
	msr	daifset, #0xf

	/* CurrentEL must report EL2; SIMA runs as a hypervisor, not as EL1 code. */
	mrs	x2, CurrentEL
	cmp	x2, #CURRENTEL_EL2
	b.ne	_start_hang
	/* SPSel=1 selects SP_EL2 as the active stack pointer at EL2. */
	msr	spsel, #1
	isb

	/* VBAR_EL2 points all EL2 exceptions to arm64_exception_vectors. */
	adrp	x2, arm64_exception_vectors
	add	x2, x2, :lo12:arm64_exception_vectors
	msr	vbar_el2, x2
	isb

	adrp	x2, _bss_start
	add	x2, x2, :lo12:_bss_start
	adrp	x3, _bss_end
	add	x3, x3, :lo12:_bss_end
1:
	cmp	x2, x3
	b.hs	2f
	str	xzr, [x2], #8
	b	1b
2:
	adrp	x4, _boot_stack_end
	add	x4, x4, :lo12:_boot_stack_end
	mov	sp, x4

	/* MPIDR_EL1 carries the CPU affinity ID used to identify the boot pCPU. */
	mrs	x0, mpidr_el1
	tst	x1, x1
	b.ne	3f
	mov	x1, xzr
3:
	bl	init_primary_pcpu
	b	_start_hang

	.align 2
	.globl _start_secondary_psci
_start_secondary_psci:
	/* Secondary CPUs enter with interrupts masked until per-CPU init is done. */
	msr	daifset, #0xf
	/* Use SP_EL2 for the scheduler/EL2 stack on secondary CPUs as well. */
	msr	spsel, #1
	isb

	/* Install the EL2 vector base before calling C code. */
	adrp	x2, arm64_exception_vectors
	add	x2, x2, :lo12:arm64_exception_vectors
	msr	vbar_el2, x2
	isb

	mov	sp, x0
	/* Pass this CPU's MPIDR affinity to init_secondary_pcpu(). */
	mrs	x0, mpidr_el1
	bl	init_secondary_pcpu
	b	_start_hang

	.align 2
	.globl _start_hang
_start_hang:
	/* WFI keeps a failed or returned boot path parked without spinning hot. */
	wfi
	b	_start_hang
