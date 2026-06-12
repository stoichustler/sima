/*
 * Copyright (C) 2026 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <irq.h>
#include <cpu.h>
#include <logmsg.h>
#include <notify.h>
#include <host_pm.h>
#include <debug/dump.h>

#include <asm/trap.h>

/*
 * EL2 trap dispatch is split by origin:
 * - host traps use this file directly and reboot after dumping state,
 * - guest exits are routed from the vector table into vcpu_exit.c.
 *
 * IRQ handling is shared so physical interrupts taken in host or guest context
 * still enter the common IRQ core through the ARM64 IRQ domain translation.
 */
void arm64_smp_call_irq_handler(__unused uint32_t irq, __unused void *data)
{
	handle_smp_call();
}

static void unexpected_trap_handler(const struct intr_excp_ctx *ctx, uint64_t trap_type)
{
	printf("unexpected arm64 trap type:%lu esr:0x%lx elr:0x%lx far:0x%lx\r\n",
		trap_type, ctx->regs.esr, ctx->regs.elr, ctx->regs.far);
	printf("──────────────── [end here] ────────────────\r\n");
	printf("arm64 trap handled: auto reboot\r\n");
	reset_host(false);
}

static void dispatch_exception(const struct intr_excp_ctx *ctx, uint64_t trap_type)
{
	uint16_t pcpu_id = get_pcpu_id();

	dump_exception(ctx, pcpu_id);
	unexpected_trap_handler(ctx, trap_type);
}

static void dispatch_interrupt_common(const struct intr_excp_ctx *ctx, bool handle_softirq)
{
	uint32_t intid = arm64_gicv3_ack_irq();
	uint32_t acrn_irq;

	/*
	 * GIC INTIDs are hardware-local. Convert them to ACRN IRQ numbers before
	 * invoking generic handlers, then EOI the physical interrupt after the
	 * handler has consumed it.
	 */
	if (intid == ARM64_GIC_SPURIOUS_INTID) {
		return;
	}

	acrn_irq = arm64_domain_get_acrn_irq(ARM64_IRQD_GIC, intid);
	if (arm64_is_valid_acrn_irq(acrn_irq)) {
		if (handle_softirq) {
			do_irq(acrn_irq);
		} else {
			do_irq_no_softirq(acrn_irq);
		}
	} else {
		unexpected_trap_handler(ctx, ARM64_TRAP_IRQ);
	}

	arm64_gicv3_eoi_irq(intid);
}

void dispatch_interrupt(const struct intr_excp_ctx *ctx)
{
	dispatch_interrupt_common(ctx, true);
}

void dispatch_interrupt_no_softirq(const struct intr_excp_ctx *ctx)
{
	dispatch_interrupt_common(ctx, false);
}

void dispatch_trap(const struct intr_excp_ctx *ctx, uint64_t trap_type)
{
	switch (trap_type) {
	case ARM64_TRAP_IRQ:
		dispatch_interrupt(ctx);
		break;
	case ARM64_TRAP_SYNC:
	case ARM64_TRAP_FIQ:
	case ARM64_TRAP_SERROR:
	default:
		dispatch_exception(ctx, trap_type);
		break;
	}
}
