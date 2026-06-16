/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_LIB_BARRIER_H
#define ARM64_LIB_BARRIER_H

static inline void arch_cpu_read_memory_barrier(void)
{
	asm volatile ("dmb ishld" ::: "memory");
}

static inline void arch_cpu_write_memory_barrier(void)
{
	asm volatile ("dmb ishst" ::: "memory");
}

static inline void arch_cpu_memory_barrier(void)
{
	asm volatile ("dmb ish" ::: "memory");
}

#endif /* ARM64_LIB_BARRIER_H */
