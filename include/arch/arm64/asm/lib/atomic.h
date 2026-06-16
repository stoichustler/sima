/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_LIB_ATOMIC_H
#define ARM64_LIB_ATOMIC_H

static inline void arch_atomic_inc32(uint32_t *ptr)
{
	(void)__atomic_add_fetch(ptr, 1U, __ATOMIC_SEQ_CST);
}

static inline void arch_atomic_inc64(uint64_t *ptr)
{
	(void)__atomic_add_fetch(ptr, 1UL, __ATOMIC_SEQ_CST);
}

static inline void arch_atomic_dec32(uint32_t *ptr)
{
	(void)__atomic_sub_fetch(ptr, 1U, __ATOMIC_SEQ_CST);
}

static inline void arch_atomic_dec64(uint64_t *ptr)
{
	(void)__atomic_sub_fetch(ptr, 1UL, __ATOMIC_SEQ_CST);
}

static inline uint32_t arch_atomic_cmpxchg32(volatile uint32_t *ptr, uint32_t old, uint32_t new)
{
	uint32_t expected = old;

	(void)__atomic_compare_exchange_n(ptr, &expected, new, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return expected;
}

static inline uint64_t arch_atomic_cmpxchg64(volatile uint64_t *ptr, uint64_t old, uint64_t new)
{
	uint64_t expected = old;

	(void)__atomic_compare_exchange_n(ptr, &expected, new, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return expected;
}

static inline uint32_t arch_atomic_swap32(uint32_t *ptr, uint32_t v)
{
	return __atomic_exchange_n(ptr, v, __ATOMIC_SEQ_CST);
}

static inline uint64_t arch_atomic_swap64(uint64_t *ptr, uint64_t v)
{
	return __atomic_exchange_n(ptr, v, __ATOMIC_SEQ_CST);
}

static inline int32_t arch_atomic_add_return(int32_t *ptr, int32_t v)
{
	return __atomic_add_fetch(ptr, v, __ATOMIC_SEQ_CST);
}

static inline int32_t arch_atomic_sub_return(int32_t *ptr, int32_t v)
{
	return __atomic_sub_fetch(ptr, v, __ATOMIC_SEQ_CST);
}

static inline int32_t arch_atomic_inc_return(int32_t *ptr)
{
	return arch_atomic_add_return(ptr, 1);
}

static inline int32_t arch_atomic_dec_return(int32_t *ptr)
{
	return arch_atomic_sub_return(ptr, 1);
}

static inline int64_t arch_atomic_add64_return(int64_t *ptr, int64_t v)
{
	return __atomic_add_fetch(ptr, v, __ATOMIC_SEQ_CST);
}

static inline int64_t arch_atomic_sub64_return(int64_t *ptr, int64_t v)
{
	return __atomic_sub_fetch(ptr, v, __ATOMIC_SEQ_CST);
}

static inline int64_t arch_atomic_inc64_return(int64_t *ptr)
{
	return arch_atomic_add64_return(ptr, 1L);
}

static inline int64_t arch_atomic_dec64_return(int64_t *ptr)
{
	return arch_atomic_sub64_return(ptr, 1L);
}

#endif /* ARM64_LIB_ATOMIC_H */
