/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_LIB_SPINLOCK_H
#define ARM64_LIB_SPINLOCK_H

#ifndef ASSEMBLER

typedef struct _arch_spinlock {
	uint64_t head;
	uint64_t tail;
} arch_spinlock_t;

static inline void arch_spinlock_obtain(arch_spinlock_t *lock)
{
	uint64_t ticket;
	uint64_t owner;

	asm volatile (
		"1:	ldaxr	%0, %2\n"
		"	add	%0, %0, #1\n"
		"	stxr	%w1, %0, %2\n"
		"	cbnz	%w1, 1b\n"
		"	sub	%0, %0, #1\n"
		: "=&r" (ticket), "=&r" (owner), "+Q" (lock->head)
		:
		: "memory");

	do {
		asm volatile ("ldar %x0, %1" : "=r" (owner) : "Q" (lock->tail) : "memory");
		if (owner != ticket) {
			arch_asm_pause();
		}
	} while (owner != ticket);
}

static inline void arch_spinlock_release(arch_spinlock_t *lock)
{
	uint64_t owner = lock->tail + 1UL;

	asm volatile ("stlr %x1, %0" : "=Q" (lock->tail) : "r" (owner) : "memory");
}

#else /* ASSEMBLER */

#define SPINLOCK_HEAD_OFFSET	0
#define SPINLOCK_TAIL_OFFSET	8

#endif /* ASSEMBLER */

#endif /* ARM64_LIB_SPINLOCK_H */
