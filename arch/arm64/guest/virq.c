/*
 * Copyright (C) 2026 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <vcpu.h>
#include <atomic.h>
#include <logmsg.h>
#include <asm/irq.h>
#include <asm/guest/virq.h>
#include <asm/guest/vgicv3.h>

void vcpu_set_trap(struct acrn_vcpu *vcpu, struct arm64_vcpu_trap_info *trap)
{
	if (trap != NULL) {
		vcpu->arch.regs.elr = trap->elr;
		vcpu->arch.regs.spsr = trap->spsr;
		vcpu->arch.regs.esr = trap->esr;
		vcpu->arch.regs.far = trap->far;
	}
}

void vcpu_queue_exception(struct acrn_vcpu *vcpu, struct arm64_vcpu_trap_info *trap)
{
	struct acrn_vcpu_arch *arch = &vcpu->arch;

	if (trap != NULL) {
		if (arch->trap.esr != EXCEPTION_INVALID) {
			pr_err("nested exception happened, prev esr=0x%lx", arch->trap.esr);
		}

		arch->trap = *trap;
		vcpu_make_request(vcpu, ARM64_VCPU_REQUEST_EXCEPTION);
	}
}

int32_t vcpu_set_intr(struct acrn_vcpu *vcpu, uint32_t hwirq)
{
	int32_t ret = -1;

	if (hwirq < ARM64_VGIC_IRQ_NUM) {
		ret = arm64_vgicv3_inject_irq(vcpu, hwirq, true);
	} else if (hwirq < BITS_PER_LONG) {
		bitmap_set(hwirq, &vcpu->arch.irqs_pending);
		bitmap_set(hwirq, &vcpu->arch.irqs_pending_mask);
		vcpu_make_request(vcpu, ARM64_VCPU_REQUEST_EVENT);
		signal_event(&vcpu->events[ARM64_VCPU_EVENT_VIRTUAL_INTERRUPT]);
		ret = 0;
	}

	return ret;
}

int32_t vcpu_clear_intr(struct acrn_vcpu *vcpu, uint32_t hwirq)
{
	int32_t ret = -1;

	if (hwirq < ARM64_VGIC_IRQ_NUM) {
		ret = arm64_vgicv3_clear_irq(vcpu, hwirq);
	} else if (hwirq < BITS_PER_LONG) {
		bitmap_clear(hwirq, &vcpu->arch.irqs_pending);
		bitmap_set(hwirq, &vcpu->arch.irqs_pending_mask);
		vcpu_make_request(vcpu, ARM64_VCPU_REQUEST_EVENT);
		ret = 0;
	}

	return ret;
}

bool vcpu_inject_pending_intr(struct acrn_vcpu *vcpu)
{
	bool injected = false;

	if (vcpu_take_request(vcpu, ARM64_VCPU_REQUEST_EVENT)) {
		(void)atomic_readandclear64(&vcpu->arch.irqs_pending_mask);
		injected = true;
	}

	return injected;
}
