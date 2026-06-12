/*
 * Copyright (C) 2026 Intel Corporation.
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
	msr	daifset, #0xf

	mrs	x2, CurrentEL
	cmp	x2, #CURRENTEL_EL2
	b.ne	_start_hang
	msr	spsel, #1
	isb

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
	msr	daifset, #0xf
	msr	spsel, #1
	isb

	adrp	x2, arm64_exception_vectors
	add	x2, x2, :lo12:arm64_exception_vectors
	msr	vbar_el2, x2
	isb

	mov	sp, x0
	mrs	x0, mpidr_el1
	bl	init_secondary_pcpu
	b	_start_hang

	.align 2
	.globl _start_hang
_start_hang:
	wfi
	b	_start_hang
