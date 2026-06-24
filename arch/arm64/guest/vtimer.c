/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2017 The FreeBSD Foundation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the company nor the name of the author may be used to
 *    endorse or promote products derived from this software without specific
 *    prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <errno.h>
#include <cpu.h>
#include <vcpu.h>
#include <vm.h>
#include <timer.h>
#include <ticks.h>
#include <asm/irq.h>
#include <asm/sysreg.h>
#include <asm/guest/vgicv3.h>

#define	VTIMER_STUCK_RESCUE_US			500U
#define	VTIMER_LR_RESCUE_RESCHED_BUDGET	4U

#define SYSREG_ENC(op0, op1, crn, crm, op2) \
	(((op0) << 20U) | ((op2) << 17U) | ((op1) << 14U) | ((crn) << 10U) | ((crm) << 1U))
#define SYSREG_CNTP_CTL_EL0		SYSREG_ENC(3UL, 3UL, 14UL, 2UL, 1UL)
#define SYSREG_CNTP_CVAL_EL0		SYSREG_ENC(3UL, 3UL, 14UL, 2UL, 2UL)
#define SYSREG_CNTP_TVAL_EL0		SYSREG_ENC(3UL, 3UL, 14UL, 2UL, 0UL)
#define SYSREG_CNTPCT_EL0		SYSREG_ENC(3UL, 3UL, 14UL, 0UL, 1UL)
#define SYSREG_CNTV_CTL_EL0		SYSREG_ENC(3UL, 3UL, 14UL, 3UL, 1UL)
#define SYSREG_CNTV_CVAL_EL0		SYSREG_ENC(3UL, 3UL, 14UL, 3UL, 2UL)
#define SYSREG_CNTV_TVAL_EL0		SYSREG_ENC(3UL, 3UL, 14UL, 3UL, 0UL)
#define SYSREG_CNTVCT_EL0		SYSREG_ENC(3UL, 3UL, 14UL, 0UL, 2UL)

static void vtimer_arm_wfi_rescue(struct acrn_vcpu *vcpu);

static uint32_t vtimer_guest_ctl(const struct arm64_vcpu_guest_ctx *gctx)
{
	return (gctx->timer_virq == ARM64_GIC_PPI_PHYSICAL_TIMER) ?
		gctx->cntp_ctl_el0 : gctx->cntv_ctl_el0;
}

static uint64_t vtimer_guest_cval(const struct arm64_vcpu_guest_ctx *gctx)
{
	return (gctx->timer_virq == ARM64_GIC_PPI_PHYSICAL_TIMER) ?
		gctx->cntp_cval_el0 : gctx->cntv_cval_el0;
}

static uint64_t vtimer_host_deadline(const struct arm64_vcpu_guest_ctx *gctx)
{
	return vtimer_guest_cval(gctx) - gctx->cntvoff_el2;
}

static bool vtimer_guest_enabled(const struct arm64_vcpu_guest_ctx *gctx)
{
	uint32_t ctl = vtimer_guest_ctl(gctx);

	return ((ctl & CNTV_CTL_ENABLE) != 0U) && ((ctl & CNTV_CTL_IMASK) == 0U);
}

static uint32_t vtimer_ctl_value(uint32_t ctl, uint64_t cval, uint64_t now)
{
	uint32_t value = ctl & (CNTV_CTL_ENABLE | CNTV_CTL_IMASK);

	if (((value & CNTV_CTL_ENABLE) != 0U) &&
		((value & CNTV_CTL_IMASK) == 0U) &&
		((int64_t)(cval - now) <= 0L)) {
		value |= CNTV_CTL_ISTATUS;
	}

	return value;
}

static bool vtimer_guest_ctx_expired(const struct arm64_vcpu_guest_ctx *gctx)
{
	return vtimer_guest_enabled(gctx) &&
		((int64_t)(vtimer_host_deadline(gctx) - cpu_ticks()) <= 0L);
}

/*
 * Keep the host PPI27 priority mask separate from the guest-visible CNTV_CTL
 * value. FreeBSD's vtimer model injects the virtual timer through the vGIC;
 * BEAU additionally priority-masks the physical virtual timer PPI while that
 * virtual line is owned by an LR.
 */
static void vtimer_set_host_mask(struct acrn_vcpu *vcpu, bool masked)
{
	struct arm64_vcpu_guest_ctx *gctx;
	struct arm64_vcpu_vtimer_diag *diag;
	uint64_t now;

	if ((vcpu == NULL) || (get_running_vcpu(get_pcpu_id()) != vcpu)) {
		return;
	}

	gctx = &vcpu->arch.gctx;
	diag = &vcpu->arch.debug.vtimer_diag;
	now = cpu_ticks();
	if (masked && !gctx->cntv_el2_masked) {
		/*
		 * The host PPI is priority-masked only while vGIC state owns an expired
		 * guest timer. Counting transitions and max age shows whether EL2 kept
		 * CNTV hidden for too long without a matching LR/EOI completion.
		 */
		gctx->cntv_el2_masked = true;
		diag->el2_mask_set++;
		diag->el2_mask_since_ticks = now;
		arm64_gicv3_set_irq_priority(ARM64_GIC_PPI_VIRTUAL_TIMER,
			ARM64_GIC_PRIORITY_MASKED);
		arm64_vcpu_trace_vtimer(vcpu, ARM64_VTIMER_TRACE_MASK,
			gctx->timer_virq, UINT32_MAX, UINT64_MAX, false, true);
	} else if (masked) {
		gctx->cntv_el2_masked = true;
	} else if (gctx->cntv_el2_masked) {
		uint64_t mask_ticks = (diag->el2_mask_since_ticks != 0UL) ?
			(now - diag->el2_mask_since_ticks) : 0UL;

		gctx->cntv_el2_masked = false;
		diag->el2_mask_clear++;
		if (mask_ticks > diag->max_el2_mask_ticks) {
			diag->max_el2_mask_ticks = mask_ticks;
		}
		diag->el2_mask_since_ticks = 0UL;
		arm64_gicv3_set_irq_priority(ARM64_GIC_PPI_VIRTUAL_TIMER,
			ARM64_GIC_PRIORITY_DEFAULT);
		arm64_vcpu_trace_vtimer(vcpu, ARM64_VTIMER_TRACE_MASK,
			gctx->timer_virq, UINT32_MAX, UINT64_MAX, false, false);
	}
}

static uint32_t vtimer_live_ctl(struct acrn_vcpu *vcpu)
{
	struct arm64_vcpu_guest_ctx *gctx = &vcpu->arch.gctx;
	uint32_t ctl = read_cntv_ctl_el0() & (CNTV_CTL_ENABLE | CNTV_CTL_IMASK);

	if (gctx->cntv_el2_masked) {
		ctl = (ctl & ~CNTV_CTL_IMASK) | (vtimer_guest_ctl(gctx) & CNTV_CTL_IMASK);
	}

	return ctl;
}

bool arm64_vtimer_sample_current(struct acrn_vcpu *vcpu)
{
	struct arm64_vcpu_guest_ctx *gctx = &vcpu->arch.gctx;
	uint32_t ctl = vtimer_live_ctl(vcpu);
	uint64_t cval = read_cntv_cval_el0();
	uint64_t now = read_cntvct_el0();

	if (gctx->timer_virq == ARM64_GIC_PPI_PHYSICAL_TIMER) {
		gctx->cntp_ctl_el0 = ctl;
		gctx->cntp_cval_el0 = cval;
	} else {
		gctx->cntv_ctl_el0 = ctl;
		gctx->cntv_cval_el0 = cval;
	}

	return (((ctl & CNTV_CTL_ENABLE) != 0U) && ((ctl & CNTV_CTL_IMASK) == 0U) &&
		((int64_t)(cval - now) <= 0L));
}

void arm64_vtimer_set_host_mask(struct acrn_vcpu *vcpu, bool masked)
{
	vtimer_set_host_mask(vcpu, masked);
}

bool arm64_vtimer_guest_expired(const struct acrn_vcpu *vcpu)
{
	return (vcpu != NULL) && vtimer_guest_ctx_expired(&vcpu->arch.gctx);
}

void arm64_vtimer_load_current(struct acrn_vcpu *vcpu)
{
	struct arm64_vcpu_guest_ctx *gctx;
	uint32_t ctl;

	if (vcpu == NULL) {
		return;
	}

	gctx = &vcpu->arch.gctx;
	write_cntv_ctl_el0(0U);
	if (gctx->cntv_el2_masked) {
		arm64_gicv3_unmask_irq(ARM64_GIC_PPI_VIRTUAL_TIMER);
		arm64_gicv3_set_irq_priority(ARM64_GIC_PPI_VIRTUAL_TIMER,
			ARM64_GIC_PRIORITY_MASKED);
	} else {
		arm64_gicv3_enable_irq(ARM64_GIC_PPI_VIRTUAL_TIMER);
		arm64_gicv3_set_irq_priority(ARM64_GIC_PPI_VIRTUAL_TIMER,
			ARM64_GIC_PRIORITY_DEFAULT);
	}
	if (gctx->timer_virq == ARM64_GIC_PPI_PHYSICAL_TIMER) {
		write_cntv_cval_el0(gctx->cntp_cval_el0);
		ctl = gctx->cntp_ctl_el0 & (CNTV_CTL_ENABLE | CNTV_CTL_IMASK);
	} else {
		write_cntv_cval_el0(gctx->cntv_cval_el0);
		ctl = gctx->cntv_ctl_el0 & (CNTV_CTL_ENABLE | CNTV_CTL_IMASK);
	}
	write_cntv_ctl_el0(ctl);
}

void arm64_vtimer_save_current(struct acrn_vcpu *vcpu)
{
	struct arm64_vcpu_guest_ctx *gctx;
	uint32_t ctl;
	uint32_t guest_ctl;
	uint64_t cval;
	uint64_t now;

	if (vcpu == NULL) {
		return;
	}

	/*
	 * Linux may access the EL1 virtual timer directly when the trap is not
	 * taken by the CPU model. Keep the guest shadow synchronized on vCPU
	 * switch-out. EL2 may temporarily gate host PPI27 while a timer interrupt
	 * is in flight; keep that host mask out of the guest shadow.
	 */
	gctx = &vcpu->arch.gctx;
	cval = read_cntv_cval_el0();
	ctl = read_cntv_ctl_el0() & (CNTV_CTL_ENABLE | CNTV_CTL_IMASK);
	guest_ctl = vtimer_guest_ctl(gctx);
	if (gctx->cntv_el2_masked) {
		ctl = (ctl & ~CNTV_CTL_IMASK) | (guest_ctl & CNTV_CTL_IMASK);
		now = read_cntvct_el0();
		if (((ctl & CNTV_CTL_ENABLE) == 0U) || ((ctl & CNTV_CTL_IMASK) != 0U) ||
			((int64_t)(cval - now) > 0L)) {
			gctx->cntv_el2_masked = false;
		}
	}
	if (gctx->timer_virq == ARM64_GIC_PPI_PHYSICAL_TIMER) {
		gctx->cntp_cval_el0 = cval;
		gctx->cntp_ctl_el0 = ctl;
	} else {
		gctx->cntv_cval_el0 = cval;
		gctx->cntv_ctl_el0 = ctl;
	}
}

void arm64_vtimer_disable_current(void)
{
	write_cntv_ctl_el0(0U);
	arm64_gicv3_enable_irq(ARM64_GIC_PPI_VIRTUAL_TIMER);
	arm64_gicv3_set_irq_priority(ARM64_GIC_PPI_VIRTUAL_TIMER,
		ARM64_GIC_PRIORITY_DEFAULT);
}

bool arm64_vtimer_sysreg(uint64_t sysreg)
{
	return (sysreg == SYSREG_CNTPCT_EL0) || (sysreg == SYSREG_CNTVCT_EL0) ||
		(sysreg == SYSREG_CNTP_CTL_EL0) || (sysreg == SYSREG_CNTP_CVAL_EL0) ||
		(sysreg == SYSREG_CNTP_TVAL_EL0) || (sysreg == SYSREG_CNTV_CTL_EL0) ||
		(sysreg == SYSREG_CNTV_CVAL_EL0) || (sysreg == SYSREG_CNTV_TVAL_EL0);
}

static bool vtimer_has_irq_state(const struct acrn_vcpu *vcpu, uint32_t virq)
{
	const struct arm64_vgic_irq *desc;

	if ((vcpu == NULL) || (vcpu->vm == NULL) ||
		(vcpu->vcpu_id >= ARM64_VGIC_MAX_VCPUS) || (virq >= ARM64_VGIC_IRQ_NUM)) {
		return false;
	}

	desc = &vcpu->vm->arch_vm.vgic.irq[vcpu->vcpu_id][virq];
	return desc->pending || desc->active;
}

void arm64_vtimer_trace_switch(struct acrn_vcpu *vcpu, uint32_t event)
{
	struct arm64_vcpu_guest_ctx *gctx;
	uint32_t virq;
	uint32_t ctl;
	uint64_t cval;
	uint64_t now;

	if (vcpu == NULL) {
		return;
	}

	gctx = &vcpu->arch.gctx;
	virq = (gctx->timer_virq == 0U) ?
		ARM64_GIC_PPI_VIRTUAL_TIMER : gctx->timer_virq;
	ctl = read_cntv_ctl_el0();
	cval = read_cntv_cval_el0();
	now = read_cntvct_el0();

	/*
	 * vCPU switches can be very frequent on shared pCPUs. Record only switch
	 * edges that could explain timer delivery: an expired deadline, EL2's
	 * temporary host mask, or an in-flight vGIC timer IRQ.
	 */
	if (gctx->cntv_el2_masked || vtimer_has_irq_state(vcpu, virq) ||
		(((ctl & CNTV_CTL_ENABLE) != 0U) && ((int64_t)(cval - now) <= 0L))) {
		arm64_vcpu_trace_vtimer(vcpu, event, virq, ctl, cval, false, false);
	}
}

static void vtimer_write_live_ctl(struct acrn_vcpu *vcpu)
{
	struct arm64_vcpu_guest_ctx *gctx = &vcpu->arch.gctx;
	uint32_t ctl = vtimer_guest_ctl(gctx);

	if (gctx->cntv_el2_masked) {
		vtimer_set_host_mask(vcpu, false);
	}
	write_cntv_ctl_el0(ctl & (CNTV_CTL_ENABLE | CNTV_CTL_IMASK));
}

int32_t arm64_vtimer_handle_sysreg(struct acrn_vcpu *vcpu, uint64_t sysreg,
	bool read, uint64_t *reg)
{
	struct arm64_vcpu_guest_ctx *gctx;
	struct arm64_vcpu_last_timer *last;
	uint64_t now = read_cntvct_el0();
	uint64_t write_value = (reg != NULL) ? *reg : 0UL;
	uint32_t write_ctl = (uint32_t)(write_value & (CNTV_CTL_ENABLE | CNTV_CTL_IMASK));
	uint64_t val;
	int32_t ret = 0;

	if (vcpu == NULL) {
		return -EINVAL;
	}

	gctx = &vcpu->arch.gctx;
	last = &vcpu->arch.debug.last_timer;
	switch (sysreg) {
	case SYSREG_CNTPCT_EL0:
		gctx->timer_virq = ARM64_GIC_PPI_PHYSICAL_TIMER;
		if (read) {
			if (reg != NULL) {
				*reg = read_cntvct_el0();
			}
		} else {
			ret = -EINVAL;
		}
		break;
	case SYSREG_CNTVCT_EL0:
		gctx->timer_virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
		if (read) {
			if (reg != NULL) {
				*reg = read_cntvct_el0();
			}
		} else {
			ret = -EINVAL;
		}
		break;
	case SYSREG_CNTP_CTL_EL0:
		gctx->timer_virq = ARM64_GIC_PPI_PHYSICAL_TIMER;
		if (read) {
			if (reg != NULL) {
				*reg = vtimer_ctl_value(gctx->cntp_ctl_el0,
					gctx->cntp_cval_el0, now);
			}
		} else {
			gctx->cntp_ctl_el0 = write_ctl;
			vtimer_write_live_ctl(vcpu);
		}
		break;
	case SYSREG_CNTV_CTL_EL0:
		gctx->timer_virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
		if (read) {
			if (reg != NULL) {
				*reg = vtimer_ctl_value(gctx->cntv_ctl_el0,
					gctx->cntv_cval_el0, now);
			}
		} else {
			gctx->cntv_ctl_el0 = write_ctl;
			vtimer_write_live_ctl(vcpu);
		}
		break;
	case SYSREG_CNTP_CVAL_EL0:
		gctx->timer_virq = ARM64_GIC_PPI_PHYSICAL_TIMER;
		if (read) {
			if (reg != NULL) {
				*reg = gctx->cntp_cval_el0;
			}
		} else {
			gctx->cntp_cval_el0 = write_value;
			write_cntv_cval_el0(gctx->cntp_cval_el0);
			vtimer_write_live_ctl(vcpu);
		}
		break;
	case SYSREG_CNTV_CVAL_EL0:
		gctx->timer_virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
		if (read) {
			if (reg != NULL) {
				*reg = gctx->cntv_cval_el0;
			}
		} else {
			gctx->cntv_cval_el0 = write_value;
			write_cntv_cval_el0(gctx->cntv_cval_el0);
			vtimer_write_live_ctl(vcpu);
		}
		break;
	case SYSREG_CNTP_TVAL_EL0:
		gctx->timer_virq = ARM64_GIC_PPI_PHYSICAL_TIMER;
		if (read) {
			if (reg != NULL) {
				*reg = (uint32_t)(gctx->cntp_cval_el0 - now);
			}
		} else {
			val = now + (uint64_t)(int32_t)(uint32_t)write_value;
			gctx->cntp_cval_el0 = val;
			write_cntv_cval_el0(val);
			vtimer_write_live_ctl(vcpu);
		}
		break;
	case SYSREG_CNTV_TVAL_EL0:
		gctx->timer_virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
		if (read) {
			if (reg != NULL) {
				*reg = (uint32_t)(gctx->cntv_cval_el0 - now);
			}
		} else {
			val = now + (uint64_t)(int32_t)(uint32_t)write_value;
			gctx->cntv_cval_el0 = val;
			write_cntv_cval_el0(val);
			vtimer_write_live_ctl(vcpu);
		}
		break;
	default:
		ret = -EINVAL;
		break;
	}

	last->cval = vtimer_guest_cval(gctx);
	last->ctl = (gctx->timer_virq == ARM64_GIC_PPI_PHYSICAL_TIMER) ?
		vtimer_ctl_value(gctx->cntp_ctl_el0, gctx->cntp_cval_el0, now) :
		vtimer_ctl_value(gctx->cntv_ctl_el0, gctx->cntv_cval_el0, now);
	last->virq = gctx->timer_virq;
	last->sysreg = (uint32_t)sysreg;
	last->status = ret;
	last->write = !read;
	last->injected = false;
	last->tsc = cpu_ticks();
	if (!read) {
		arm64_vcpu_trace_vtimer(vcpu, ARM64_VTIMER_TRACE_SYSREG,
			last->virq, last->ctl, last->cval, true, false);
	}

	return ret;
}

void arm64_vtimer_arm_wfi_rescue(struct acrn_vcpu *vcpu)
{
	if (vcpu != NULL) {
		vtimer_arm_wfi_rescue(vcpu);
	}
}

static void vtimer_disarm_backup(struct acrn_vcpu *vcpu)
{
	if ((vcpu != NULL) && vcpu->arch.vtimer_backup_initialized) {
		if (timer_is_started(&vcpu->arch.vtimer_backup)) {
			del_timer(&vcpu->arch.vtimer_backup);
		}
		update_timer(&vcpu->arch.vtimer_backup, 0UL, 0UL);
		vcpu->arch.vtimer_stuck_rescue_armed = false;
	}
}

static void vtimer_arm_backup(struct acrn_vcpu *vcpu)
{
	struct arm64_vcpu_guest_ctx *gctx;
	uint64_t deadline;

	if ((vcpu == NULL) || !vcpu->arch.vtimer_backup_initialized) {
		return;
	}

	vcpu->arch.vtimer_stuck_rescue_armed = false;
	gctx = &vcpu->arch.gctx;
	deadline = vtimer_host_deadline(gctx);
	if (!vtimer_guest_enabled(gctx) || ((int64_t)(deadline - cpu_ticks()) <= 0L)) {
		vtimer_disarm_backup(vcpu);
		return;
	}

	if (timer_is_started(&vcpu->arch.vtimer_backup)) {
		del_timer(&vcpu->arch.vtimer_backup);
	}
	update_timer(&vcpu->arch.vtimer_backup, deadline, 0UL);
	if (add_timer(&vcpu->arch.vtimer_backup) != 0) {
		update_timer(&vcpu->arch.vtimer_backup, 0UL, 0UL);
	}
}

static void vtimer_arm_stuck_rescue(struct acrn_vcpu *vcpu)
{
	uint64_t deadline;

	if ((vcpu == NULL) || !vcpu->arch.vtimer_backup_initialized) {
		return;
	}
	if (vcpu->arch.vtimer_stuck_rescue_armed) {
		return;
	}

	deadline = cpu_ticks() + us_to_ticks(VTIMER_STUCK_RESCUE_US);
	if (timer_is_started(&vcpu->arch.vtimer_backup)) {
		del_timer(&vcpu->arch.vtimer_backup);
	}
	update_timer(&vcpu->arch.vtimer_backup, deadline, 0UL);
	if (add_timer(&vcpu->arch.vtimer_backup) != 0) {
		update_timer(&vcpu->arch.vtimer_backup, 0UL, 0UL);
		vcpu->arch.vtimer_stuck_rescue_armed = false;
	} else {
		vcpu->arch.vtimer_stuck_rescue_armed = true;
	}
}

static void vtimer_arm_wfi_rescue(struct acrn_vcpu *vcpu)
{
	vcpu->arch.debug.vtimer_diag.wfi_rescue_arm++;
	vcpu->arch.vtimer_wfi_rescue = true;
	vcpu->arch.gctx.hcr_el2 |= HCR_TWI;
	if (get_running_vcpu(get_pcpu_id()) == vcpu) {
		write_hcr_el2(vcpu->arch.gctx.hcr_el2);
	}
	arm64_vcpu_trace_vtimer(vcpu, ARM64_VTIMER_TRACE_WFI,
		vcpu->arch.gctx.timer_virq, UINT32_MAX, UINT64_MAX, false, true);
}

static void vtimer_keep_rescue(struct acrn_vcpu *vcpu)
{
	vcpu->arch.vtimer_wfi_rescue = false;
	if (!vcpu->arch.vtimer_lr_rescue) {
		vcpu->arch.vtimer_lr_rescue_budget = VTIMER_LR_RESCUE_RESCHED_BUDGET;
	}
	vcpu->arch.vtimer_lr_rescue = true;
	vcpu->arch.gctx.hcr_el2 &= ~HCR_TWI;
	if (get_running_vcpu(get_pcpu_id()) == vcpu) {
		write_hcr_el2(vcpu->arch.gctx.hcr_el2);
	}
}

static void vtimer_clear_rescue(struct acrn_vcpu *vcpu)
{
	vcpu->arch.vtimer_wfi_rescue = false;
	vcpu->arch.vtimer_lr_rescue = false;
	vcpu->arch.vtimer_lr_rescue_budget = 0U;
	vcpu->arch.gctx.hcr_el2 &= ~HCR_TWI;
	if (get_running_vcpu(get_pcpu_id()) == vcpu) {
		write_hcr_el2(vcpu->arch.gctx.hcr_el2);
	}
}

static int32_t vtimer_inject_current(struct acrn_vcpu *vcpu, uint32_t virq,
	uint32_t guest_ctl, uint64_t guest_cval)
{
	int32_t ret;

	vtimer_disarm_backup(vcpu);
	ret = arm64_vgicv3_inject_irq(vcpu, virq, true);
	vcpu->arch.debug.last_timer.cval = guest_cval;
	vcpu->arch.debug.last_timer.ctl = (guest_ctl & (CNTV_CTL_ENABLE | CNTV_CTL_IMASK)) |
		CNTV_CTL_ISTATUS;
	vcpu->arch.debug.last_timer.virq = virq;
	vcpu->arch.debug.last_timer.sysreg = 0U;
	vcpu->arch.debug.last_timer.status = ret;
	vcpu->arch.debug.last_timer.write = false;
	vcpu->arch.debug.last_timer.injected = true;
	vcpu->arch.debug.last_timer.tsc = cpu_ticks();
	arm64_vcpu_trace_vtimer(vcpu, ARM64_VTIMER_TRACE_INJECT, virq,
		vcpu->arch.debug.last_timer.ctl, guest_cval, false, true);

	return ret;
}

static void vtimer_backup_handler(void *data)
{
	struct acrn_vcpu *vcpu = (struct acrn_vcpu *)data;
	struct arm64_vcpu_guest_ctx *gctx;
	uint32_t ctl;
	uint32_t virq;

	if ((vcpu == NULL) || (vcpu->vm == NULL) || (vcpu->state != VCPU_RUNNING)) {
		return;
	}
	vcpu->arch.vtimer_stuck_rescue_armed = false;
	if (get_running_vcpu(get_pcpu_id()) == vcpu) {
		if (!arm64_vgicv3_timer_live_stuck(vcpu)) {
			return;
		}
		gctx = &vcpu->arch.gctx;
		virq = (gctx->timer_virq == 0U) ? ARM64_GIC_PPI_VIRTUAL_TIMER : gctx->timer_virq;
		ctl = vtimer_guest_ctl(gctx);
		arm64_vcpu_trace_vtimer(vcpu, ARM64_VTIMER_TRACE_BACKUP, virq,
			ctl, vtimer_guest_cval(gctx), false, false);
		vtimer_arm_wfi_rescue(vcpu);
		return;
	}

	gctx = &vcpu->arch.gctx;
	virq = (gctx->timer_virq == 0U) ? ARM64_GIC_PPI_VIRTUAL_TIMER : gctx->timer_virq;
	ctl = vtimer_guest_ctl(gctx);
	if (!vtimer_guest_ctx_expired(gctx)) {
		return;
	}

	arm64_vcpu_trace_vtimer(vcpu, ARM64_VTIMER_TRACE_BACKUP, virq,
		ctl, vtimer_guest_cval(gctx), false, false);
	(void)vtimer_inject_current(vcpu, virq, ctl, vtimer_guest_cval(gctx));
}

void arm64_vtimer_init_vcpu(struct acrn_vcpu *vcpu)
{
	initialize_timer(&vcpu->arch.vtimer_backup, vtimer_backup_handler,
		vcpu, 0UL, 0UL);
	vcpu->arch.vtimer_backup_initialized = true;
}

void arm64_vgicv3_arm_vtimer_backup(struct acrn_vcpu *vcpu)
{
	vtimer_arm_backup(vcpu);
}

void arm64_vgicv3_cancel_vtimer_backup(struct acrn_vcpu *vcpu)
{
	vtimer_disarm_backup(vcpu);
}

void arm64_vgicv3_keep_vtimer_rescue(struct acrn_vcpu *vcpu)
{
	if (vcpu != NULL) {
		vtimer_keep_rescue(vcpu);
	}
}

void arm64_vgicv3_clear_vtimer_rescue(struct acrn_vcpu *vcpu)
{
	if (vcpu != NULL) {
		vtimer_clear_rescue(vcpu);
	}
}

void arm64_vgicv3_update_current_vtimer(struct acrn_vcpu *vcpu)
{
	uint32_t virq;
	bool level;
	bool rescue;

	if ((vcpu == NULL) || (vcpu->vm == NULL) || (get_running_vcpu(get_pcpu_id()) != vcpu)) {
		return;
	}

	virq = vcpu->arch.gctx.timer_virq;
	if (virq == 0U) {
		virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
		vcpu->arch.gctx.timer_virq = virq;
	}

	level = arm64_vtimer_sample_current(vcpu);
	arm64_vgicv3_sync_current_timer_line(vcpu, level);
	if (level || vcpu->arch.gctx.cntv_el2_masked) {
		arm64_vcpu_trace_vtimer(vcpu, ARM64_VTIMER_TRACE_UPDATE, virq,
			UINT32_MAX, UINT64_MAX, false, false);
	}

	rescue = arm64_vgicv3_timer_live_stuck(vcpu);
	if (rescue) {
		vtimer_arm_wfi_rescue(vcpu);
		vtimer_arm_stuck_rescue(vcpu);
	}
}

void arm64_vgicv3_virtual_timer_irq_handler(__unused uint32_t irq, __unused void *data)
{
	struct acrn_vcpu *vcpu = get_running_vcpu(get_pcpu_id());

	if (vcpu != NULL) {
		uint32_t virq = vcpu->arch.gctx.timer_virq;
		uint32_t guest_ctl;
		uint64_t guest_cval;

		if (virq == 0U) {
			vcpu->arch.gctx.timer_virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
			virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
		}
		if (virq == ARM64_GIC_PPI_PHYSICAL_TIMER) {
			vcpu->arch.gctx.cntp_cval_el0 = read_cntv_cval_el0();
			vcpu->arch.gctx.cntp_ctl_el0 = vtimer_live_ctl(vcpu);
		} else {
			vcpu->arch.gctx.cntv_cval_el0 = read_cntv_cval_el0();
			vcpu->arch.gctx.cntv_ctl_el0 = vtimer_live_ctl(vcpu);
		}

		vtimer_set_host_mask(vcpu, true);
		if (virq == ARM64_GIC_PPI_PHYSICAL_TIMER) {
			guest_ctl = vcpu->arch.gctx.cntp_ctl_el0;
			guest_cval = vcpu->arch.gctx.cntp_cval_el0;
		} else {
			guest_ctl = vcpu->arch.gctx.cntv_ctl_el0;
			guest_cval = vcpu->arch.gctx.cntv_cval_el0;
		}
		arm64_vcpu_trace_vtimer(vcpu, ARM64_VTIMER_TRACE_PPI, virq,
			guest_ctl, guest_cval, false, false);
		(void)vtimer_inject_current(vcpu, virq, guest_ctl, guest_cval);
	}
}

void arm64_vgicv3_poll_current_vtimer(struct acrn_vcpu *vcpu)
{
	uint32_t ctl;
	uint64_t cval;
	uint64_t now;

	if ((vcpu == NULL) || (get_running_vcpu(get_pcpu_id()) != vcpu)) {
		return;
	}

	if (vcpu->arch.gctx.cntv_el2_masked) {
		(void)arm64_vgicv3_requeue_lost_masked_timer(vcpu);
		return;
	}
	ctl = vtimer_live_ctl(vcpu);
	cval = read_cntv_cval_el0();

	if (((ctl & CNTV_CTL_ENABLE) == 0U) || ((ctl & CNTV_CTL_IMASK) != 0U)) {
		return;
	}

	now = read_cntvct_el0();
	if ((int64_t)(cval - now) > 0L) {
		return;
	}

	if (vcpu->arch.gctx.timer_virq == 0U) {
		vcpu->arch.gctx.timer_virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
	}
	if (vcpu->arch.gctx.timer_virq == ARM64_GIC_PPI_PHYSICAL_TIMER) {
		vcpu->arch.gctx.cntp_cval_el0 = cval;
		vcpu->arch.gctx.cntp_ctl_el0 = ctl & (CNTV_CTL_ENABLE | CNTV_CTL_IMASK);
	} else {
		vcpu->arch.gctx.cntv_cval_el0 = cval;
		vcpu->arch.gctx.cntv_ctl_el0 = ctl & (CNTV_CTL_ENABLE | CNTV_CTL_IMASK);
	}
	vtimer_set_host_mask(vcpu, true);
	arm64_vcpu_trace_vtimer(vcpu, ARM64_VTIMER_TRACE_POLL,
		vcpu->arch.gctx.timer_virq, ctl, cval, false, false);
	if (vcpu->arch.gctx.timer_virq == ARM64_GIC_PPI_PHYSICAL_TIMER) {
		(void)vtimer_inject_current(vcpu, ARM64_GIC_PPI_PHYSICAL_TIMER,
			vcpu->arch.gctx.cntp_ctl_el0, cval);
	} else {
		(void)vtimer_inject_current(vcpu, ARM64_GIC_PPI_VIRTUAL_TIMER,
			vcpu->arch.gctx.cntv_ctl_el0, cval);
	}
}
