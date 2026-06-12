/*
 * Copyright (C) 2026 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <irq.h>
#include <schedule.h>
#include <cpu.h>
#include <logmsg.h>
#include <asm/irq.h>
#include <asm/trap.h>

void arch_init_smp_call(void)
{
	uint32_t acrn_irq;

	if (get_pcpu_id() == BSP_CPU_ID) {
		acrn_irq = arm64_domain_get_acrn_irq(ARM64_IRQD_GIC, ARM64_GIC_SGI_SMP_CALL);
		if ((acrn_irq == IRQ_INVALID) ||
			(request_irq(acrn_irq, arm64_smp_call_irq_handler, NULL, IRQF_NONE) < 0)) {
			pr_err("software interrupt irq setup failed");
		}
	}
}

void arch_smp_call_kick_pcpu(uint16_t pcpu_id)
{
	arm64_gicv3_send_sgi(pcpu_id, ARM64_GIC_SGI_SMP_CALL);
}

void arch_send_reschedule_request(uint16_t pcpu_id)
{
	arch_smp_call_kick_pcpu(pcpu_id);
}
