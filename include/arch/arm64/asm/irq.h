/*
 * Copyright (C) 2026 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_IRQ_H
#define ARM64_IRQ_H

#include <types.h>
#include <cpu.h>

#define IPI_NOTIFY_CPU		0U
#define EXCEPTION_INVALID	0xffffffffffffffffUL

#define ARM64_IRQ_SRC_VIRTUAL_TIMER	0U
#define ARM64_IRQ_SRC_IPI		1U
#define ARM64_IRQ_SRC_EXTERNAL		2U
#define IRQ_NUM_CPU_DOMAIN		16U

#define ARM64_GIC_SGI_SMP_CALL		0U
#define ARM64_GIC_PPI_VGIC_MAINTENANCE	25U
#define ARM64_GIC_PPI_PHYSICAL_TIMER	30U
#define ARM64_GIC_PPI_VIRTUAL_TIMER	27U
#define ARM64_GIC_SPURIOUS_INTID	1023U

/*
 * Reserved space for the future GIC distributor domain. The first-stage
 * skeleton only wires the CPU-local domain.
 */
#define IRQ_NUM_GIC_DOMAIN		1024U
#define NR_IRQS				(IRQ_NUM_CPU_DOMAIN + IRQ_NUM_GIC_DOMAIN)

struct arm64_irq_data {
	uint32_t acrn_irq;
};

struct intr_excp_ctx {
	struct cpu_regs regs;
};

#define ARM64_IRQD_CPU		"cpu-intc"
#define ARM64_IRQD_GIC		"gicv3"

bool arm64_register_irq_domain(const char *name, uint32_t irq_num);
uint32_t arm64_domain_get_acrn_irq(const char *name, uint32_t src_id);
bool arm64_is_valid_acrn_irq(uint32_t irq);
void arm64_gicv3_init_early(void);
void arm64_gicv3_init(uint16_t pcpu_id);
uint32_t arm64_gicv3_ack_irq(void);
void arm64_gicv3_eoi_irq(uint32_t intid);
void arm64_gicv3_enable_irq(uint32_t intid);
void arm64_gicv3_send_sgi(uint16_t pcpu_id, uint32_t sgi_id);

#endif /* ARM64_IRQ_H */
