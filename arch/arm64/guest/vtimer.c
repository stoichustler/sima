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

/*
 * BEAU deliberately keeps the two EL1 timer views separate:
 *
 *   CNTV_*  -> guest virtual timer, interrupt PPI27
 *   CNTP_*  -> guest physical timer, interrupt PPI30
 *
 * CNTP access must never steal the live CNTV comparator or change which guest
 * interrupt a CNTV expiry injects. Linux normally uses CNTV/PPI27 for its
 * clockevent, while some smaller guests probe or use CNTP/PPI30. Sharing one
 * "active timer_virq" between both paths lets those probes misroute the next
 * CNTV interrupt and eventually stops guest ticks.
 *
 * CNTV path
 * ---------
 * The pCPU has one live CNTV register bank. On vCPU load BEAU restores the
 * vCPU's saved CNTV_CTL/CVAL into the hardware CNTV registers. When hardware
 * PPI27 fires, EL2 snapshots CNTV state, masks the live CNTV comparator with
 * CNTV_CTL.IMASK while the vGIC owns the level line, and injects guest PPI27.
 * On vCPU unload, BEAU saves the live CNTV registers back into the vCPU shadow
 * and disables live CNTV so the next vCPU cannot inherit this deadline.
 *
 * If a vCPU is not loaded, cntv_timer is armed at
 *
 *     host CNTPCT deadline = guest CNTV_CVAL + CNTVOFF_EL2
 *
 * because CNTVCT_EL0 is architecturally CNTPCT_EL0 - CNTVOFF_EL2.
 *
 * CNTP path
 * ---------
 * EL1 physical timer registers are trapped. BEAU stores CNTP_CTL/CVAL in the
 * vCPU shadow and arms cntp_timer when the emulated CNTP is
 * enabled and unmasked. When that host timer expires, BEAU injects guest PPI30.
 * CNTP is software-emulated and cannot affect the live CNTV/PPI27 delivery
 * path.
 *
 * CNTP_CVAL is already in the physical-counter domain, so its host deadline is
 * the value written by the guest. CNTVOFF applies only to CNTV/CNTVCT.
 *
 * The field gctx->timer_virq is kept only as a debug/compatibility hint for
 * older dump paths. The timer delivery model itself does not depend on it.
 */
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

static uint64_t vtimer_virtual_now(const struct arm64_vcpu_guest_ctx *gctx)
{
	return cpu_ticks() - gctx->cntvoff_el2;
}

static uint64_t vtimer_virtual_deadline(const struct arm64_vcpu_guest_ctx *gctx,
	uint64_t guest_cval)
{
	/* CNTVCT = CNTPCT - CNTVOFF, so host CNTPCT deadline = guest CVAL + CNTVOFF. */
	return guest_cval + gctx->cntvoff_el2;
}

static bool vtimer_ctl_enabled(uint32_t ctl)
{
	return ((ctl & CNTV_CTL_ENABLE) != 0U) && ((ctl & CNTV_CTL_IMASK) == 0U);
}

static bool vtimer_source_expired(uint32_t ctl, uint64_t cval, uint64_t now)
{
	return vtimer_ctl_enabled(ctl) && ((int64_t)(cval - now) <= 0L);
}

static bool vtimer_virtual_expired(const struct arm64_vcpu_guest_ctx *gctx)
{
	return vtimer_source_expired(gctx->cntv_ctl_el0, gctx->cntv_cval_el0,
		vtimer_virtual_now(gctx));
}

static bool vtimer_physical_expired(const struct arm64_vcpu_guest_ctx *gctx)
{
	return vtimer_source_expired(gctx->cntp_ctl_el0, gctx->cntp_cval_el0,
		cpu_ticks());
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
	return vtimer_virtual_expired(gctx);
}

static void vtimer_record_last(struct acrn_vcpu *vcpu, uint32_t virq,
	uint32_t ctl, uint64_t cval, uint32_t sysreg, int32_t status,
	bool write, bool injected)
{
	struct arm64_vcpu_last_timer *last = &vcpu->arch.debug.last_timer;

	last->cval = cval;
	last->ctl = ctl;
	last->virq = virq;
	last->sysreg = sysreg;
	last->status = status;
	last->write = write;
	last->injected = injected;
	last->tsc = cpu_ticks();
}

static void cntp_timer_disarm(struct acrn_vcpu *vcpu)
{
	if ((vcpu != NULL) && vcpu->arch.cntp_timer_initialized) {
		if (timer_is_started(&vcpu->arch.cntp_timer)) {
			del_timer(&vcpu->arch.cntp_timer);
		}
		update_timer(&vcpu->arch.cntp_timer, 0UL, 0UL);
	}
}

static int32_t vtimer_inject_current(struct acrn_vcpu *vcpu, uint32_t virq,
	uint32_t guest_ctl, uint64_t guest_cval);

static void cntp_timer_arm(struct acrn_vcpu *vcpu);
static void cntp_timer_sync_line(struct acrn_vcpu *vcpu);

static void cntp_timer_handler(void *data)
{
	struct acrn_vcpu *vcpu = (struct acrn_vcpu *)data;
	struct arm64_vcpu_guest_ctx *gctx;
	uint32_t ctl;
	uint64_t cval;

	if ((vcpu == NULL) || (vcpu->vm == NULL) || (vcpu->state != VCPU_RUNNING)) {
		return;
	}

	gctx = &vcpu->arch.gctx;
	if (!vtimer_physical_expired(gctx)) {
		return;
	}

	ctl = vtimer_ctl_value(gctx->cntp_ctl_el0, gctx->cntp_cval_el0, cpu_ticks());
	cval = gctx->cntp_cval_el0;
	gctx->timer_virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
	arm64_vcpu_trace_vtimer(vcpu, ARM64_VTIMER_TRACE_BACKUP,
		ARM64_GIC_PPI_PHYSICAL_TIMER, ctl, cval, false, false);
	(void)vtimer_inject_current(vcpu, ARM64_GIC_PPI_PHYSICAL_TIMER, ctl, cval);
}

static void cntp_timer_update(struct acrn_vcpu *vcpu, bool deassert_inactive)
{
	struct arm64_vcpu_guest_ctx *gctx;
	uint64_t deadline;

	if ((vcpu == NULL) || !vcpu->arch.cntp_timer_initialized) {
		return;
	}

	gctx = &vcpu->arch.gctx;
	if (!vtimer_ctl_enabled(gctx->cntp_ctl_el0)) {
		cntp_timer_disarm(vcpu);
		if (deassert_inactive) {
			(void)arm64_vgicv3_deassert_irq(vcpu, ARM64_GIC_PPI_PHYSICAL_TIMER);
		}
		return;
	}

	deadline = gctx->cntp_cval_el0;
	if ((int64_t)(deadline - cpu_ticks()) <= 0L) {
		cntp_timer_disarm(vcpu);
		(void)cntp_timer_handler(vcpu);
		return;
	}

	if (deassert_inactive) {
		(void)arm64_vgicv3_deassert_irq(vcpu, ARM64_GIC_PPI_PHYSICAL_TIMER);
	}
	if (timer_is_started(&vcpu->arch.cntp_timer)) {
		del_timer(&vcpu->arch.cntp_timer);
	}
	update_timer(&vcpu->arch.cntp_timer, deadline, 0UL);
	if (add_timer(&vcpu->arch.cntp_timer) != 0) {
		update_timer(&vcpu->arch.cntp_timer, 0UL, 0UL);
	}
}

static void cntp_timer_arm(struct acrn_vcpu *vcpu)
{
	cntp_timer_update(vcpu, false);
}

static void cntp_timer_sync_line(struct acrn_vcpu *vcpu)
{
	cntp_timer_update(vcpu, true);
}

/*
 * Keep EL2's live CNTV mask separate from the guest-visible CNTV_CTL value.
 * When EL2 owns an expired CNTV line, BEAU masks only the live comparator with
 * CNTV_CTL.IMASK. The guest shadow keeps the guest's original IMASK bit, so a
 * guest CNTV read/write still observes its own timer state.
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
		 * The live CNTV comparator is masked only while vGIC state owns an
		 * expired guest timer. Counting transitions and max age shows whether
		 * EL2 kept CNTV hidden for too long without matching LR/EOI progress.
		 */
		gctx->cntv_el2_masked = true;
		diag->el2_mask_set++;
		diag->el2_mask_since_ticks = now;
		write_cntv_ctl_el0((read_cntv_ctl_el0() | CNTV_CTL_IMASK) &
			(CNTV_CTL_ENABLE | CNTV_CTL_IMASK));
		arm64_vcpu_trace_vtimer(vcpu, ARM64_VTIMER_TRACE_MASK,
			ARM64_GIC_PPI_VIRTUAL_TIMER, UINT32_MAX, UINT64_MAX, false, true);
	} else if (masked) {
		gctx->cntv_el2_masked = true;
		write_cntv_ctl_el0((read_cntv_ctl_el0() | CNTV_CTL_IMASK) &
			(CNTV_CTL_ENABLE | CNTV_CTL_IMASK));
	} else if (gctx->cntv_el2_masked) {
		uint64_t mask_ticks = (diag->el2_mask_since_ticks != 0UL) ?
			(now - diag->el2_mask_since_ticks) : 0UL;

		gctx->cntv_el2_masked = false;
		diag->el2_mask_clear++;
		if (mask_ticks > diag->max_el2_mask_ticks) {
			diag->max_el2_mask_ticks = mask_ticks;
		}
		diag->el2_mask_since_ticks = 0UL;
		write_cntv_ctl_el0(gctx->cntv_ctl_el0 & (CNTV_CTL_ENABLE | CNTV_CTL_IMASK));
		arm64_vcpu_trace_vtimer(vcpu, ARM64_VTIMER_TRACE_MASK,
			ARM64_GIC_PPI_VIRTUAL_TIMER, UINT32_MAX, UINT64_MAX, false, false);
	}
}

static uint32_t vtimer_live_ctl(struct acrn_vcpu *vcpu)
{
	struct arm64_vcpu_guest_ctx *gctx = &vcpu->arch.gctx;
	uint32_t ctl = read_cntv_ctl_el0() & (CNTV_CTL_ENABLE | CNTV_CTL_IMASK);

	if (gctx->cntv_el2_masked) {
		ctl = (ctl & ~CNTV_CTL_IMASK) | (gctx->cntv_ctl_el0 & CNTV_CTL_IMASK);
	}

	return ctl;
}

bool arm64_vtimer_sample_current(struct acrn_vcpu *vcpu)
{
	struct arm64_vcpu_guest_ctx *gctx = &vcpu->arch.gctx;
	uint32_t ctl = vtimer_live_ctl(vcpu);
	uint64_t cval = read_cntv_cval_el0();
	uint64_t now = read_cntvct_el0();

	gctx->cntv_ctl_el0 = ctl;
	gctx->cntv_cval_el0 = cval;

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

	if (vcpu == NULL) {
		return;
	}

	gctx = &vcpu->arch.gctx;
	/*
	 * Load path:
	 *
	 *   saved CNTV shadow   -> live CNTV registers
	 *   saved EL2 host mask -> live CNTV_CTL.IMASK
	 *
	 * CNTP is not loaded into hardware here. It is emulated by cntp_timer_arm()
	 * using the vCPU's saved CNTP shadow.
	 */
	gctx->timer_virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
	write_cntv_ctl_el0(0U);
	arm64_gicv3_enable_irq(ARM64_GIC_PPI_VIRTUAL_TIMER);
	arm64_gicv3_set_irq_priority(ARM64_GIC_PPI_VIRTUAL_TIMER,
		ARM64_GIC_PRIORITY_DEFAULT);
	write_cntv_cval_el0(gctx->cntv_cval_el0);
	write_cntv_ctl_el0((gctx->cntv_ctl_el0 |
		(gctx->cntv_el2_masked ? CNTV_CTL_IMASK : 0U)) &
		(CNTV_CTL_ENABLE | CNTV_CTL_IMASK));
	cntp_timer_arm(vcpu);
}

void arm64_vtimer_save_current(struct acrn_vcpu *vcpu)
{
	struct arm64_vcpu_guest_ctx *gctx;
	uint32_t ctl;
	uint64_t cval;
	uint64_t now;

	if (vcpu == NULL) {
		return;
	}

	/*
	 * Save path:
	 *
	 *   live CNTV registers -> saved CNTV shadow
	 *   live CNTV_CTL.IMASK -> EL2-only ownership state
	 *
	 * The CNTP shadow is already updated by trapped CNTP accesses and by its
	 * own host software timer. Saving live CNTV must never overwrite it.
	 */
	gctx = &vcpu->arch.gctx;
	cval = read_cntv_cval_el0();
	ctl = read_cntv_ctl_el0() & (CNTV_CTL_ENABLE | CNTV_CTL_IMASK);
	if (gctx->cntv_el2_masked) {
		ctl = (ctl & ~CNTV_CTL_IMASK) | (gctx->cntv_ctl_el0 & CNTV_CTL_IMASK);
		now = read_cntvct_el0();
		if (((ctl & CNTV_CTL_ENABLE) == 0U) || ((ctl & CNTV_CTL_IMASK) != 0U) ||
			((int64_t)(cval - now) > 0L)) {
			gctx->cntv_el2_masked = false;
		}
	}
	gctx->cntv_cval_el0 = cval;
	gctx->cntv_ctl_el0 = ctl;
	cntp_timer_arm(vcpu);
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

static uint32_t vtimer_sysreg_virq(uint64_t sysreg,
	__unused const struct arm64_vcpu_guest_ctx *gctx)
{
	switch (sysreg) {
	case SYSREG_CNTP_CTL_EL0:
	case SYSREG_CNTP_CVAL_EL0:
	case SYSREG_CNTP_TVAL_EL0:
		return ARM64_GIC_PPI_PHYSICAL_TIMER;
	case SYSREG_CNTV_CTL_EL0:
	case SYSREG_CNTV_CVAL_EL0:
	case SYSREG_CNTV_TVAL_EL0:
		return ARM64_GIC_PPI_VIRTUAL_TIMER;
	default:
		return ARM64_GIC_PPI_VIRTUAL_TIMER;
	}
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
	virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
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

static void vtimer_load_live_cntv(struct acrn_vcpu *vcpu)
{
	struct arm64_vcpu_guest_ctx *gctx = &vcpu->arch.gctx;
	bool was_masked = gctx->cntv_el2_masked;

	write_cntv_ctl_el0(0U);
	write_cntv_cval_el0(gctx->cntv_cval_el0);
	if (was_masked) {
		vtimer_set_host_mask(vcpu, false);
	} else {
		write_cntv_ctl_el0(gctx->cntv_ctl_el0 & (CNTV_CTL_ENABLE | CNTV_CTL_IMASK));
	}
}

int32_t arm64_vtimer_handle_sysreg(struct acrn_vcpu *vcpu, uint64_t sysreg,
	bool read, uint64_t *reg)
{
	struct arm64_vcpu_guest_ctx *gctx;
	uint64_t cntv_now;
	uint64_t cntp_now;
	uint64_t write_value = (reg != NULL) ? *reg : 0UL;
	uint32_t write_ctl = (uint32_t)(write_value & (CNTV_CTL_ENABLE | CNTV_CTL_IMASK));
	uint32_t access_virq;
	uint32_t ctl;
	uint64_t cval;
	uint64_t val;
	int32_t ret = 0;

	if (vcpu == NULL) {
		return -EINVAL;
	}

	/*
	 * Sysreg emulation is the guest-controlled side of timer ownership:
	 *
	 * - CNTV accesses update the virtual timer shadow and, for writes, refresh
	 *   the live CNTV comparator used for PPI27 delivery.
	 * - CNTP accesses update only the physical timer shadow and arm/disarm the
	 *   separate software cntp_timer that injects PPI30.
	 */
	gctx = &vcpu->arch.gctx;
	cntv_now = vtimer_virtual_now(gctx);
	cntp_now = cpu_ticks();
	access_virq = vtimer_sysreg_virq(sysreg, gctx);
	switch (sysreg) {
	case SYSREG_CNTPCT_EL0:
		if (read) {
			if (reg != NULL) {
				*reg = cntp_now;
			}
		} else {
			ret = -EINVAL;
		}
		break;
	case SYSREG_CNTVCT_EL0:
		if (read) {
			if (reg != NULL) {
				*reg = read_cntvct_el0();
			}
		} else {
			ret = -EINVAL;
		}
		break;
	case SYSREG_CNTP_CTL_EL0:
		if (read) {
			if (reg != NULL) {
				*reg = vtimer_ctl_value(gctx->cntp_ctl_el0,
					gctx->cntp_cval_el0, cntp_now);
			}
		} else {
			gctx->cntp_ctl_el0 = write_ctl;
		}
		break;
	case SYSREG_CNTV_CTL_EL0:
		if (read) {
			if (reg != NULL) {
				*reg = vtimer_ctl_value(gctx->cntv_ctl_el0,
					gctx->cntv_cval_el0, cntv_now);
			}
		} else {
			gctx->cntv_ctl_el0 = write_ctl;
		}
		break;
	case SYSREG_CNTP_CVAL_EL0:
		if (read) {
			if (reg != NULL) {
				*reg = gctx->cntp_cval_el0;
			}
		} else {
			gctx->cntp_cval_el0 = write_value;
		}
		break;
	case SYSREG_CNTV_CVAL_EL0:
		if (read) {
			if (reg != NULL) {
				*reg = gctx->cntv_cval_el0;
			}
		} else {
			gctx->cntv_cval_el0 = write_value;
		}
		break;
	case SYSREG_CNTP_TVAL_EL0:
		if (read) {
			if (reg != NULL) {
				*reg = (uint32_t)(gctx->cntp_cval_el0 - cntp_now);
			}
		} else {
			val = cntp_now + (uint64_t)(int32_t)(uint32_t)write_value;
			gctx->cntp_cval_el0 = val;
		}
		break;
	case SYSREG_CNTV_TVAL_EL0:
		if (read) {
			if (reg != NULL) {
				*reg = (uint32_t)(gctx->cntv_cval_el0 - cntv_now);
			}
		} else {
			val = cntv_now + (uint64_t)(int32_t)(uint32_t)write_value;
			gctx->cntv_cval_el0 = val;
		}
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (access_virq == ARM64_GIC_PPI_PHYSICAL_TIMER) {
		ctl = vtimer_ctl_value(gctx->cntp_ctl_el0, gctx->cntp_cval_el0, cntp_now);
		cval = gctx->cntp_cval_el0;
	} else {
		ctl = vtimer_ctl_value(gctx->cntv_ctl_el0, gctx->cntv_cval_el0, cntv_now);
		cval = gctx->cntv_cval_el0;
	}
	vtimer_record_last(vcpu, access_virq, ctl, cval, (uint32_t)sysreg, ret, !read, false);
	if (!read && (ret == 0)) {
		if (access_virq == ARM64_GIC_PPI_PHYSICAL_TIMER) {
			cntp_timer_sync_line(vcpu);
		} else {
			gctx->timer_virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
			vtimer_load_live_cntv(vcpu);
		}
		arm64_vcpu_trace_vtimer(vcpu, ARM64_VTIMER_TRACE_SYSREG,
			access_virq, ctl, cval, true, false);
	}

	return ret;
}

static void cntv_timer_disarm(struct acrn_vcpu *vcpu)
{
	if ((vcpu != NULL) && vcpu->arch.cntv_timer_initialized) {
		if (timer_is_started(&vcpu->arch.cntv_timer)) {
			del_timer(&vcpu->arch.cntv_timer);
		}
		update_timer(&vcpu->arch.cntv_timer, 0UL, 0UL);
	}
}

static void cntv_timer_arm(struct acrn_vcpu *vcpu)
{
	struct arm64_vcpu_guest_ctx *gctx;
	uint64_t deadline;

	if ((vcpu == NULL) || !vcpu->arch.cntv_timer_initialized) {
		return;
	}

	/*
	 * Backup timer covers the offline case: the vCPU is not running on this
	 * pCPU, so EL2 arms a host timer for the saved CNTV deadline and later
	 * rebuilds a normal PPI27 injection if the deadline expires.
	 */
	gctx = &vcpu->arch.gctx;
	deadline = vtimer_virtual_deadline(gctx, gctx->cntv_cval_el0);
	if (!vtimer_ctl_enabled(gctx->cntv_ctl_el0) ||
		((int64_t)(deadline - cpu_ticks()) <= 0L)) {
		cntv_timer_disarm(vcpu);
		return;
	}

	if (timer_is_started(&vcpu->arch.cntv_timer)) {
		del_timer(&vcpu->arch.cntv_timer);
	}
	update_timer(&vcpu->arch.cntv_timer, deadline, 0UL);
	if (add_timer(&vcpu->arch.cntv_timer) != 0) {
		update_timer(&vcpu->arch.cntv_timer, 0UL, 0UL);
	}
}

static int32_t vtimer_inject_current(struct acrn_vcpu *vcpu, uint32_t virq,
	uint32_t guest_ctl, uint64_t guest_cval)
{
	int32_t ret;

	/*
	 * Injection is the single boundary between timer expiry and guest-visible
	 * PPIs. PPI27 comes from live/saved CNTV; PPI30 comes from the software
	 * CNTP emulation. Keep the debug shadow next to the vGIC request.
	 */
	if (virq == ARM64_GIC_PPI_VIRTUAL_TIMER) {
		cntv_timer_disarm(vcpu);
	} else if (virq == ARM64_GIC_PPI_PHYSICAL_TIMER) {
		cntp_timer_disarm(vcpu);
	}
	ret = arm64_vgicv3_inject_irq(vcpu, virq, true);
	vtimer_record_last(vcpu, virq,
		(guest_ctl & (CNTV_CTL_ENABLE | CNTV_CTL_IMASK)) | CNTV_CTL_ISTATUS,
		guest_cval, 0U, ret, false, true);
	arm64_vcpu_trace_vtimer(vcpu, ARM64_VTIMER_TRACE_INJECT, virq,
		vcpu->arch.debug.last_timer.ctl, guest_cval, false, true);

	return ret;
}

static void cntv_timer_handler(void *data)
{
	struct acrn_vcpu *vcpu = (struct acrn_vcpu *)data;
	struct arm64_vcpu_guest_ctx *gctx;
	uint32_t ctl;
	uint64_t cval;

	if ((vcpu == NULL) || (vcpu->vm == NULL) || (vcpu->state != VCPU_RUNNING)) {
		return;
	}
	if (get_running_vcpu(get_pcpu_id()) == vcpu) {
		return;
	}

	gctx = &vcpu->arch.gctx;
	if (!vtimer_guest_ctx_expired(gctx)) {
		return;
	}
	ctl = vtimer_ctl_value(gctx->cntv_ctl_el0, gctx->cntv_cval_el0,
		vtimer_virtual_now(gctx));
	cval = gctx->cntv_cval_el0;

	/*
	 * Offline backup has no live LR to inspect. If the saved guest deadline is
	 * still expired, inject through the same vGIC path as a host PPI27 hit.
	 */
	arm64_vcpu_trace_vtimer(vcpu, ARM64_VTIMER_TRACE_BACKUP,
		ARM64_GIC_PPI_VIRTUAL_TIMER, ctl, cval, false, false);
	(void)vtimer_inject_current(vcpu, ARM64_GIC_PPI_VIRTUAL_TIMER, ctl, cval);
}

void arm64_vtimer_init_vcpu(struct acrn_vcpu *vcpu)
{
	if (vcpu == NULL) {
		return;
	}
	cntv_timer_disarm(vcpu);
	cntp_timer_disarm(vcpu);
	initialize_timer(&vcpu->arch.cntv_timer, cntv_timer_handler,
		vcpu, 0UL, 0UL);
	initialize_timer(&vcpu->arch.cntp_timer, cntp_timer_handler, vcpu, 0UL, 0UL);
	vcpu->arch.cntv_timer_initialized = true;
	vcpu->arch.cntp_timer_initialized = true;
}

void arm64_vgicv3_arm_cntv_timer(struct acrn_vcpu *vcpu)
{
	cntv_timer_arm(vcpu);
}

void arm64_vgicv3_cancel_cntv_timer(struct acrn_vcpu *vcpu)
{
	cntv_timer_disarm(vcpu);
}

void arm64_vtimer_cancel_all(struct acrn_vcpu *vcpu)
{
	cntv_timer_disarm(vcpu);
	cntp_timer_disarm(vcpu);
}

void arm64_vgicv3_update_current_vtimer(struct acrn_vcpu *vcpu)
{
	bool level;

	if ((vcpu == NULL) || (vcpu->vm == NULL) || (get_running_vcpu(get_pcpu_id()) != vcpu)) {
		return;
	}

	vcpu->arch.gctx.timer_virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
	cntp_timer_arm(vcpu);

	/*
	 * Update path at a bounded EL2 sync point:
	 *
	 *   live CNTV sample -> vGIC timer line
	 */
	level = arm64_vtimer_sample_current(vcpu);
	arm64_vgicv3_sync_current_timer_line(vcpu, level);
	if (level || vcpu->arch.gctx.cntv_el2_masked) {
		arm64_vcpu_trace_vtimer(vcpu, ARM64_VTIMER_TRACE_UPDATE,
			ARM64_GIC_PPI_VIRTUAL_TIMER, UINT32_MAX, UINT64_MAX, false, false);
	}
}

void arm64_vgicv3_virtual_timer_irq_handler(__unused uint32_t irq, __unused void *data)
{
	struct acrn_vcpu *vcpu = get_running_vcpu(get_pcpu_id());

	if (vcpu != NULL) {
		uint32_t guest_ctl;
		uint64_t guest_cval;

		vcpu->arch.gctx.timer_virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
		/*
		 * Host PPI27 means the loaded guest timer fired. EL2 snapshots
		 * live CNTV, masks the live comparator with CNTV_CTL.IMASK, and
		 * asks vGIC to present guest PPI27.
		 */
		vcpu->arch.gctx.cntv_cval_el0 = read_cntv_cval_el0();
		vcpu->arch.gctx.cntv_ctl_el0 = vtimer_live_ctl(vcpu);

		vtimer_set_host_mask(vcpu, true);
		guest_ctl = vcpu->arch.gctx.cntv_ctl_el0;
		guest_cval = vcpu->arch.gctx.cntv_cval_el0;
		arm64_vcpu_trace_vtimer(vcpu, ARM64_VTIMER_TRACE_PPI,
			ARM64_GIC_PPI_VIRTUAL_TIMER, guest_ctl, guest_cval, false, false);
		(void)vtimer_inject_current(vcpu, ARM64_GIC_PPI_VIRTUAL_TIMER,
			guest_ctl, guest_cval);
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

	cntp_timer_arm(vcpu);
	/*
	 * Polling is not a background timer loop. It is used at bounded exit/return
	 * points to rebuild guest-visible delivery if CNTV expired without a host
	 * PPI path being taken.
	 */
	ctl = vtimer_live_ctl(vcpu);
	cval = read_cntv_cval_el0();

	if (((ctl & CNTV_CTL_ENABLE) == 0U) || ((ctl & CNTV_CTL_IMASK) != 0U)) {
		return;
	}

	now = read_cntvct_el0();
	if ((int64_t)(cval - now) > 0L) {
		return;
	}

	vcpu->arch.gctx.timer_virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
	vcpu->arch.gctx.cntv_cval_el0 = cval;
	vcpu->arch.gctx.cntv_ctl_el0 = ctl & (CNTV_CTL_ENABLE | CNTV_CTL_IMASK);
	vtimer_set_host_mask(vcpu, true);
	arm64_vcpu_trace_vtimer(vcpu, ARM64_VTIMER_TRACE_POLL,
		ARM64_GIC_PPI_VIRTUAL_TIMER, ctl, cval, false, false);
	(void)vtimer_inject_current(vcpu, ARM64_GIC_PPI_VIRTUAL_TIMER,
		vcpu->arch.gctx.cntv_ctl_el0, cval);
}
