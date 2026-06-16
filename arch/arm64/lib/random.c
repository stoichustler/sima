/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <asm/sysreg.h>

uint64_t arch_get_random_value(void)
{
	uint64_t cnt = read_cntpct_el0();
	uint64_t mpidr = read_mpidr_el1();

	return cnt ^ (mpidr << 17U);
}
