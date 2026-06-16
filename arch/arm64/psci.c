/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <asm/psci.h>

#define PSCI_0_2_FN_BASE		0x84000000UL
#define PSCI_0_2_64BIT			0x40000000UL
#define PSCI_0_2_FN64_BASE		(PSCI_0_2_FN_BASE + PSCI_0_2_64BIT)
#define PSCI_0_2_FN64(n)		(PSCI_0_2_FN64_BASE + (n))
#define PSCI_0_2_FN(n)			(PSCI_0_2_FN_BASE + (n))
#define PSCI_0_2_FN64_CPU_ON		PSCI_0_2_FN64(3UL)
#define PSCI_0_2_FN_SYSTEM_OFF		PSCI_0_2_FN(8UL)
#define PSCI_0_2_FN_SYSTEM_RESET	PSCI_0_2_FN(9UL)

static uint64_t arm_smccc_smc(uint64_t function_id, uint64_t arg0, uint64_t arg1, uint64_t arg2)
{
	register uint64_t x0 asm("x0") = function_id;
	register uint64_t x1 asm("x1") = arg0;
	register uint64_t x2 asm("x2") = arg1;
	register uint64_t x3 asm("x3") = arg2;

	asm volatile ("smc #0"
		: "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
		:
		: "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12",
		  "x13", "x14", "x15", "x16", "x17", "memory");

	return x0;
}

int64_t psci_cpu_on(uint64_t target_cpu, uint64_t entry_point, uint64_t context_id)
{
	return (int64_t)arm_smccc_smc(PSCI_0_2_FN64_CPU_ON, target_cpu, entry_point, context_id);
}

int64_t psci_system_off(void)
{
	return (int64_t)arm_smccc_smc(PSCI_0_2_FN_SYSTEM_OFF, 0UL, 0UL, 0UL);
}

int64_t psci_system_reset(void)
{
	return (int64_t)arm_smccc_smc(PSCI_0_2_FN_SYSTEM_RESET, 0UL, 0UL, 0UL);
}
