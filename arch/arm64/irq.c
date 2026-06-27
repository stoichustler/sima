/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <irq.h>
#include <rtl.h>
#include <spinlock.h>
#include <asm/trap.h>
#include <asm/sysreg.h>
#include <asm/guest/vgicv3.h>

static spinlock_t arm64_irq_lock = { .head = 0U, .tail = 0U };
static struct arm64_irq_data irq_data[NR_IRQS];

#define MAX_IRQ_DOMAIN_NUM		2U
#define MAX_IRQ_DOMAIN_NAME_SIZE	32U

/*
 * ARM64 separates source-local interrupt numbers from generic ACRN IRQ
 * numbers. Domains reserve contiguous ACRN IRQ ranges for CPU-local sources
 * and GIC INTIDs so common IRQ code can stay architecture-neutral.
 */
struct arm64_irq_domain {
	char name[MAX_IRQ_DOMAIN_NAME_SIZE];
	uint32_t base;
	uint32_t irq_num;
};

static struct arm64_irq_domain arm64_domains[MAX_IRQ_DOMAIN_NUM];
static uint32_t nr_domains;

static struct arm64_irq_domain *find_domain_by_name(const char *name)
{
	uint32_t i;
	struct arm64_irq_domain *domain = NULL;

	if (name == NULL) {
		pr_err("%s: invalid name", __func__);
	} else {
		for (i = 0U; i < nr_domains; i++) {
			if (strncmp(arm64_domains[i].name, name, MAX_IRQ_DOMAIN_NAME_SIZE) == 0) {
				domain = &arm64_domains[i];
				break;
			}
		}
	}

	return domain;
}

uint32_t arm64_domain_get_acrn_irq(const char *name, uint32_t src_id)
{
	uint32_t acrn_irq = IRQ_INVALID;
	struct arm64_irq_domain *domain = find_domain_by_name(name);

	if ((domain != NULL) && (src_id < domain->irq_num)) {
		acrn_irq = domain->base + src_id;
	} else {
		pr_err("%s: invalid params name=%s src_id=%u", __func__, name, src_id);
	}

	return acrn_irq;
}

bool arm64_register_irq_domain(const char *name, uint32_t irq_num)
{
	static uint32_t next_acrn_irq_base;
	bool ret = false;
	uint32_t base, end, i, acrn_irq;
	uint64_t flags;
	struct arm64_irq_domain *domain;

	/*
	 * Domains are allocated once during IRQ setup. Reserving every translated
	 * ACRN IRQ immediately catches collisions before drivers start requesting
	 * handlers.
	 */
	if (find_domain_by_name(name) != NULL) {
		pr_err("%s: domain %s already exists", __func__, name);
	} else if (nr_domains >= MAX_IRQ_DOMAIN_NUM) {
		pr_err("%s: too many irq domains", __func__);
	} else {
		spinlock_irqsave_obtain(&arm64_irq_lock, &flags);

		base = next_acrn_irq_base;
		end = base + irq_num;
		if ((irq_num > 0U) && (end <= NR_IRQS)) {
			domain = &arm64_domains[nr_domains];
			(void)strncpy_s(domain->name, MAX_IRQ_DOMAIN_NAME_SIZE, name, (MAX_IRQ_DOMAIN_NAME_SIZE - 1U));
			domain->base = base;
			domain->irq_num = irq_num;
			nr_domains++;
			next_acrn_irq_base = end;
			ret = true;
		} else {
			pr_err("%s: invalid irq range base=%u irq_num=%u", __func__, base, irq_num);
		}

		spinlock_irqrestore_release(&arm64_irq_lock, flags);
	}

	if (ret) {
		for (i = 0U; i < irq_num; i++) {
			acrn_irq = arm64_domain_get_acrn_irq(name, i);
			if ((acrn_irq == IRQ_INVALID) || (reserve_irq_num(acrn_irq) == IRQ_INVALID)) {
				pr_err("%s: failed to reserve irq[%u]", __func__, acrn_irq);
				ret = false;
				break;
			}
		}
	}

	return ret;
}

bool arch_request_irq(uint32_t irq)
{
	bool ret = false;
	uint64_t flags;
	struct arm64_irq_domain *gic_domain;

	/*
	 * Generic code requests an ACRN IRQ. If that IRQ belongs to the GIC domain,
	 * translate it back to a physical INTID and enable the corresponding real
	 * interrupt in the GIC.
	 */
	spinlock_irqsave_obtain(&arm64_irq_lock, &flags);

	if (irq < NR_IRQS) {
		irq_data[irq].acrn_irq = irq;
		ret = true;
	} else {
		pr_err("invalid irq=%u", irq);
	}

	spinlock_irqrestore_release(&arm64_irq_lock, flags);

	if (ret) {
		gic_domain = find_domain_by_name(ARM64_IRQD_GIC);
		if ((gic_domain != NULL) && (irq >= gic_domain->base) &&
			(irq < (gic_domain->base + gic_domain->irq_num))) {
			arm64_gicv3_enable_irq(irq - gic_domain->base);
		}
	}

	return ret;
}

void arch_free_irq(uint32_t irq)
{
	uint64_t flags;

	spinlock_irqsave_obtain(&arm64_irq_lock, &flags);

	if (irq < NR_IRQS) {
		irq_data[irq].acrn_irq = IRQ_INVALID;
	} else {
		pr_err("invalid irq=%u", irq);
	}

	spinlock_irqrestore_release(&arm64_irq_lock, flags);
}

void arch_pre_irq(__unused const struct irq_desc *desc)
{
}

void arch_post_irq(__unused const struct irq_desc *desc)
{
}

static const char *gic_irq_name(uint32_t intid)
{
	const char *name;

	switch (intid) {
	case ARM64_GIC_SGI_SMP_CALL:
		name = "gic:sgi-smp";
		break;
	case ARM64_GIC_PPI_VGIC_MAINTENANCE:
		name = "gic:vgic-mirq";
		break;
	case ARM64_GIC_PPI_HYPERVISOR_TIMER:
		name = "gic:cnthp-ppi";
		break;
	case ARM64_GIC_PPI_VIRTUAL_TIMER:
		/* This is the host CNTV PPI entry count, not the guest tick count. */
		name = "gic:cntv-ppi";
		break;
	case ARM64_GIC_PPI_PHYSICAL_TIMER:
		name = "gic:ptimer";
		break;
	default:
		if (intid < 16U) {
			name = "gic:sgi";
		} else if (intid < 32U) {
			name = "gic:ppi";
		} else if (intid < ARM64_GIC_SPURIOUS_INTID) {
			name = "gic:spi";
		} else if (intid == ARM64_GIC_SPURIOUS_INTID) {
			name = "gic:spurious";
		} else {
			name = "-";
		}
		break;
	}

	return name;
}

const char *arch_irq_name(uint32_t irq)
{
	struct arm64_irq_domain *domain;
	const char *name = "-";

	/*
	 * irqstat prints generic ACRN IRQ numbers, while ARM hardware reports GIC
	 * INTIDs. Decode the domain range here so the shell can explain whether a
	 * count belongs to an SGI, PPI, SPI, or one of the EL2-owned timer/vGIC
	 * sources without teaching common shell code about ARM64 numbering.
	 */
	domain = find_domain_by_name(ARM64_IRQD_CPU);
	if ((domain != NULL) && (irq >= domain->base) &&
		(irq < (domain->base + domain->irq_num))) {
		name = "cpu-local";
	}

	domain = find_domain_by_name(ARM64_IRQD_GIC);
	if ((domain != NULL) && (irq >= domain->base) &&
		(irq < (domain->base + domain->irq_num))) {
		name = gic_irq_name(irq - domain->base);
	}

	return name;
}

void arch_init_irq_descs(struct irq_desc descs[])
{
	uint32_t i;

	for (i = 0U; i < NR_IRQS; i++) {
		irq_data[i].acrn_irq = IRQ_INVALID;
		descs[i].arch_data = &irq_data[i];
	}
}

void arch_setup_irqs(void)
{
	/*
	 * The host needs two always-on guest-related interrupt sources: vGIC
	 * maintenance for LR lifecycle events and the physical virtual timer PPI
	 * that is re-injected as a guest virtual timer interrupt.
	 */
	(void)arm64_register_irq_domain(ARM64_IRQD_CPU, IRQ_NUM_CPU_DOMAIN);
	(void)arm64_register_irq_domain(ARM64_IRQD_GIC, IRQ_NUM_GIC_DOMAIN);
	(void)request_irq(arm64_domain_get_acrn_irq(ARM64_IRQD_GIC, ARM64_VGIC_MAINTENANCE_INTID),
		arm64_vgicv3_maintenance_irq_handler, NULL, IRQF_NONE);
	(void)request_irq(arm64_domain_get_acrn_irq(ARM64_IRQD_GIC, ARM64_GIC_PPI_VIRTUAL_TIMER),
		arm64_vgicv3_virtual_timer_irq_handler, NULL, IRQF_NONE);
}

void arch_init_interrupt(uint16_t pcpu_id)
{
	write_vbar_el2((uint64_t)arm64_exception_vectors);
	arm64_gicv3_init(pcpu_id);
}

bool arm64_is_valid_acrn_irq(uint32_t irq)
{
	bool ret = false;

	if (irq < NR_IRQS) {
		ret = (irq_data[irq].acrn_irq == irq);
	}

	return ret;
}
