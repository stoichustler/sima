/*
 * Copyright (C) 2026 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_SECURITY_H
#define ARM64_SECURITY_H

#ifndef ASSEMBLER

#include <types.h>
#include <random.h>

#ifdef STACK_PROTECTOR
extern unsigned long __stack_chk_guard;

static inline __attribute__((__always_inline__)) void init_stack_canary(void)
{
	__stack_chk_guard = get_random_value();
}
#endif

static inline bool check_cpu_security_cap(void)
{
	return true;
}

static inline void cpu_internal_buffers_clear(void)
{
}

static inline bool is_ept_force_4k_ipage(void)
{
	return false;
}

#endif /* ASSEMBLER */

#endif /* ARM64_SECURITY_H */
