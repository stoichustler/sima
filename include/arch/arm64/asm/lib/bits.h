/*
 * Copyright (C) 2026 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_LIB_BITS_H
#define ARM64_LIB_BITS_H

#include <types.h>
#include <asm/cpu.h>

#define INVALID_BIT_INDEX	0xffffU

static inline uint16_t arch_ffs64(uint64_t x)
{
	return (x == 0UL) ? INVALID_BIT_INDEX : (uint16_t)__builtin_ctzll(x);
}

static inline uint16_t arch_fls64(uint64_t x)
{
	return (x == 0UL) ? INVALID_BIT_INDEX : (uint16_t)(BITS_PER_LONG - 1U - __builtin_clzll(x));
}

static inline int16_t arch_fls32(uint32_t x)
{
	return (x == 0U) ? (int16_t)INVALID_BIT_INDEX : (int16_t)(31U - __builtin_clz(x));
}

static inline bool arch_bitmap_test(uint32_t nr, const volatile uint64_t *addr)
{
	uint64_t mask = 1UL << (nr & 63U);

	return ((*addr & mask) != 0UL);
}

static inline bool arch_bitmap32_test(uint32_t nr, const volatile uint32_t *addr)
{
	uint32_t mask = 1U << (nr & 31U);

	return ((*addr & mask) != 0U);
}

static inline void arch_bitmap_set(uint32_t nr, volatile uint64_t *addr)
{
	uint64_t mask = 1UL << (nr & 63U);

	(void)__atomic_fetch_or(addr, mask, __ATOMIC_SEQ_CST);
}

static inline void arch_bitmap32_set(uint32_t nr, volatile uint32_t *addr)
{
	uint32_t mask = 1U << (nr & 31U);

	(void)__atomic_fetch_or(addr, mask, __ATOMIC_SEQ_CST);
}

static inline void arch_bitmap_clear(uint32_t nr, volatile uint64_t *addr)
{
	uint64_t mask = ~(1UL << (nr & 63U));

	(void)__atomic_fetch_and(addr, mask, __ATOMIC_SEQ_CST);
}

static inline void arch_bitmap32_clear(uint32_t nr, volatile uint32_t *addr)
{
	uint32_t mask = ~(1U << (nr & 31U));

	(void)__atomic_fetch_and(addr, mask, __ATOMIC_SEQ_CST);
}

static inline bool arch_bitmap_test_and_set(uint32_t nr, volatile uint64_t *addr)
{
	uint64_t mask = 1UL << (nr & 63U);
	uint64_t old = __atomic_fetch_or(addr, mask, __ATOMIC_SEQ_CST);

	return ((old & mask) != 0UL);
}

static inline bool arch_bitmap32_test_and_set(uint32_t nr, volatile uint32_t *addr)
{
	uint32_t mask = 1U << (nr & 31U);
	uint32_t old = __atomic_fetch_or(addr, mask, __ATOMIC_SEQ_CST);

	return ((old & mask) != 0U);
}

static inline bool arch_bitmap_test_and_clear(uint32_t nr, volatile uint64_t *addr)
{
	uint64_t mask = 1UL << (nr & 63U);
	uint64_t old = __atomic_fetch_and(addr, ~mask, __ATOMIC_SEQ_CST);

	return ((old & mask) != 0UL);
}

static inline bool arch_bitmap32_test_and_clear(uint32_t nr, volatile uint32_t *addr)
{
	uint32_t mask = 1U << (nr & 31U);
	uint32_t old = __atomic_fetch_and(addr, ~mask, __ATOMIC_SEQ_CST);

	return ((old & mask) != 0U);
}

#endif /* ARM64_LIB_BITS_H */
