/*
 * Copyright (C) 2026 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <softirq.h>
#include <timer.h>
#include <irq.h>
#include <cpu.h>
#include <logmsg.h>
#include <asm/sysreg.h>
#include <asm/irq.h>

#define ARM64_TIMER_STOP	UINT32_MAX

static void timer_irq_handler(__unused uint32_t irq, __unused void *data)
{
	arch_set_timer_count(ARM64_TIMER_STOP);
	fire_softirq(SOFTIRQ_TIMER);
}

void arch_init_timer(void)
{
	uint32_t acrn_irq;

	/*
	 * The scheduler uses the ARM physical timer as a per-pCPU tick source.
	 * Architecturally the timer interrupt is a PPI: the IRQ action is a single
	 * global descriptor, but every pCPU has its own enable bit and comparator.
	 * Install the common handler on the BSP once, then enable and stop the
	 * local PPI on every pCPU as it initializes.
	 */
	acrn_irq = arm64_domain_get_acrn_irq(ARM64_IRQD_GIC, ARM64_GIC_PPI_PHYSICAL_TIMER);
	if (get_pcpu_id() == BSP_CPU_ID) {
		if ((acrn_irq == IRQ_INVALID) || (request_irq(acrn_irq, timer_irq_handler, NULL, IRQF_NONE) < 0)) {
			pr_err("timer irq setup failed");
		}
	}
	if (acrn_irq != IRQ_INVALID) {
		arm64_gicv3_enable_irq(ARM64_GIC_PPI_PHYSICAL_TIMER);
		arch_set_timer_count(ARM64_TIMER_STOP);
	}
}

uint64_t arch_cpu_ticks(void)
{
	return read_cntpct_el0();
}

uint32_t arch_cpu_tickrate(void)
{
	return read_cntfrq_el0() / 1000U;
}

void arch_set_timer_count(uint64_t timeout)
{
	uint64_t now = arch_cpu_ticks();
	uint64_t delta = (timeout > now) ? (timeout - now) : 1UL;

	if (delta > UINT32_MAX) {
		delta = UINT32_MAX;
	}

	write_cntp_ctl_el0(0U);
	write_cntp_tval_el0((uint32_t)delta);
	write_cntp_ctl_el0(CNTV_CTL_ENABLE);
}
