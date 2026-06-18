/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <vcpu.h>
#include <vm.h>
#include <event.h>
#include <errno.h>
#include <logmsg.h>
#include <schedule.h>
#include <ticks.h>
#include <asm/irq.h>
#include <asm/cpu.h>
#include <asm/sysreg.h>
#include <asm/guest/vcpu_priv.h>
#include <asm/guest/virq.h>
#include <asm/guest/vgicv3.h>

/*
 * CPU virtualization keeps two layers of state:
 * - vcpu->arch.regs is the persistent guest register image.
 * - gctx/vgic fields are EL2 control state loaded around scheduling.
 *
 * The scheduler treats each vCPU as a thread. Context switch hooks program the
 * EL2 virtualization registers before the thread enters the guest and save
 * them back when the thread is switched out.
 */
#define SPSR_EL2_MODE_EL1H	0x5UL
#define ARM64_GUEST_SCTLR_EL1_INIT	0x00c50078UL

void vcpu_set_elr(struct acrn_vcpu *vcpu, uint64_t val)
{
	vcpu->arch.regs.elr = val;
}

static void arm64_init_guest_regs(struct cpu_regs *regs, uint64_t entry, uint64_t x0)
{
	uint64_t host_tpidr = regs->host_tpidr;
	uint64_t exc_sp = regs->exc_sp;

	/*
	 * Keep the EL2-private return fields intact; guest-visible GPRs and
	 * exception state are reset to the Linux boot ABI before EL1 entry.
	 */
	(void)memset(regs, 0U, sizeof(*regs));
	regs->host_tpidr = host_tpidr;
	regs->exc_sp = exc_sp;
	regs->x0 = x0;
	regs->elr = entry;
	regs->spsr = SPSR_EL2_MODE_EL1H | DAIF_ALL;
}

static void arm64_init_guest_control_context(struct acrn_vcpu *vcpu)
{
	struct arm64_vcpu_guest_ctx *gctx = &vcpu->arch.gctx;

	/*
	 * Linux enters with EL1 MMU/data cache disabled and with architected EL1
	 * state known, not inherited from the pCPU that happened to create the
	 * vCPU. Values here mirror the reset-style context used by established ARM
	 * hypervisors while keeping BEAU's stage-2 and timer virtualization state.
	 */
	(void)memset(gctx, 0U, sizeof(*gctx));
	gctx->vttbr_el2 = hva2hpa(vcpu->vm->root_stg2ptp);
	gctx->vtcr_el2 = VTCR_EL2_VALUE;
	gctx->hcr_el2 = HCR_VM | HCR_RW | HCR_IMO | HCR_FMO | HCR_AMO | HCR_TSC;
	gctx->cntvoff_el2 = (uint64_t)vcpu->vm->arch_vm.time_delta;
	gctx->timer_virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
	gctx->sctlr_el1 = ARM64_GUEST_SCTLR_EL1_INIT;
}

void arm64_prepare_linux_vcpu_context(struct acrn_vcpu *vcpu, uint64_t entry, uint64_t x0)
{
	if ((vcpu == NULL) || (vcpu->vm == NULL)) {
		return;
	}

	arm64_vgicv3_cancel_vtimer_backup(vcpu);
	vcpu->pending_req = 0UL;
	vcpu->arch.irqs_pending = 0UL;
	vcpu->arch.irqs_pending_mask = 0UL;
	vcpu->arch.trap.esr = EXCEPTION_INVALID;
	arm64_init_guest_regs(&vcpu->arch.regs, entry, x0);
	arm64_init_guest_control_context(vcpu);
	arm64_vgicv3_reset_vcpu_boot_state(vcpu);
}

static uint32_t arm64_vcpu_timer_shadow_ctl(const struct arm64_vcpu_guest_ctx *gctx,
	uint32_t virq)
{
	return (virq == ARM64_GIC_PPI_PHYSICAL_TIMER) ?
		gctx->cntp_ctl_el0 : gctx->cntv_ctl_el0;
}

static uint64_t arm64_vcpu_timer_shadow_cval(const struct arm64_vcpu_guest_ctx *gctx,
	uint32_t virq)
{
	return (virq == ARM64_GIC_PPI_PHYSICAL_TIMER) ?
		gctx->cntp_cval_el0 : gctx->cntv_cval_el0;
}

static bool arm64_vcpu_vtimer_has_irq_state(const struct acrn_vcpu *vcpu, uint32_t virq)
{
	const struct arm64_vgic_irq *desc;

	if ((vcpu->vcpu_id >= ARM64_VGIC_MAX_VCPUS) || (virq >= ARM64_VGIC_IRQ_NUM)) {
		return false;
	}

	desc = &vcpu->vm->arch_vm.vgic.irq[vcpu->vcpu_id][virq];
	return desc->pending || desc->active;
}

void arm64_vcpu_trace_vtimer(struct acrn_vcpu *vcpu, uint32_t event,
	uint32_t virq, uint32_t ctl, uint64_t cval, bool write, bool injected)
{
	struct arm64_vcpu_vtimer_trace *trace;
	struct arm64_vcpu_vtimer_trace_entry *entry;
	const struct arm64_vcpu_guest_ctx *gctx;
	const struct arm64_vgicv3_vcpu_ctx *vgic_ctx;
	const struct arm64_vgic_irq *desc = NULL;
	uint32_t idx;
	uint64_t now;
	bool current;

	if ((vcpu == NULL) || (vcpu->vm == NULL)) {
		return;
	}

	gctx = &vcpu->arch.gctx;
	vgic_ctx = &vcpu->arch.vgic;
	trace = &vcpu->arch.debug.vtimer_trace;
	if (virq == 0U) {
		virq = (gctx->timer_virq == 0U) ?
			ARM64_GIC_PPI_VIRTUAL_TIMER : gctx->timer_virq;
	}

	current = (get_running_vcpu(get_pcpu_id()) == vcpu);
	now = current ? read_cntvct_el0() : (cpu_ticks() + gctx->cntvoff_el2);
	if (ctl == UINT32_MAX) {
		ctl = current ? read_cntv_ctl_el0() : arm64_vcpu_timer_shadow_ctl(gctx, virq);
	}
	if (cval == UINT64_MAX) {
		cval = current ? read_cntv_cval_el0() : arm64_vcpu_timer_shadow_cval(gctx, virq);
	}
	if ((vcpu->vcpu_id < ARM64_VGIC_MAX_VCPUS) && (virq < ARM64_VGIC_IRQ_NUM)) {
		desc = &vcpu->vm->arch_vm.vgic.irq[vcpu->vcpu_id][virq];
	}

	idx = trace->head;
	if (idx >= ARM64_VCPU_VTIMER_TRACE_NUM) {
		idx = 0U;
	}
	entry = &trace->entry[idx];
	entry->tsc = cpu_ticks();
	entry->cntvct = now;
	entry->cval = cval;
	entry->ctl = ctl & (CNTV_CTL_ENABLE | CNTV_CTL_IMASK | CNTV_CTL_ISTATUS);
	entry->virq = virq;
	entry->pcpu_id = get_pcpu_id();
	entry->event = (uint8_t)event;
	entry->used_lrs = vgic_ctx->used_lrs;
	entry->hcr = current ? read_ich_hcr_el2() : vgic_ctx->hcr;
	entry->misr = current ? read_ich_misr_el2() : 0UL;
	entry->lr0 = current ? read_ich_lr_el2(0U) :
		((vgic_ctx->used_lrs > 0U) ? vgic_ctx->lr[0U] : 0UL);
	entry->expired = (((entry->ctl & CNTV_CTL_ENABLE) != 0U) &&
		((int64_t)(cval - now) <= 0L));
	entry->masked = gctx->cntv_el2_masked;
	entry->pending = (desc != NULL) ? desc->pending : false;
	entry->active = (desc != NULL) ? desc->active : false;
	entry->level = (desc != NULL) ? desc->level : false;
	entry->write = write;
	entry->injected = injected;

	idx++;
	if (idx >= ARM64_VCPU_VTIMER_TRACE_NUM) {
		idx = 0U;
	}
	trace->head = idx;
	if (trace->count < ARM64_VCPU_VTIMER_TRACE_NUM) {
		trace->count++;
	}
}

static void arm64_vcpu_trace_vtimer_switch(struct acrn_vcpu *vcpu, uint32_t event)
{
	struct arm64_vcpu_guest_ctx *gctx = &vcpu->arch.gctx;
	uint32_t virq = (gctx->timer_virq == 0U) ?
		ARM64_GIC_PPI_VIRTUAL_TIMER : gctx->timer_virq;
	uint32_t ctl = read_cntv_ctl_el0();
	uint64_t cval = read_cntv_cval_el0();
	uint64_t now = read_cntvct_el0();

	/*
	 * vCPU switches can be very frequent on shared pCPUs. Record only switch
	 * edges that could explain timer delivery: an expired deadline, EL2's
	 * temporary host mask, or an in-flight vGIC timer IRQ.
	 */
	if (gctx->cntv_el2_masked || arm64_vcpu_vtimer_has_irq_state(vcpu, virq) ||
		(((ctl & CNTV_CTL_ENABLE) != 0U) && ((int64_t)(cval - now) <= 0L))) {
		arm64_vcpu_trace_vtimer(vcpu, event, virq, ctl, cval, false, false);
	}
}

/*
 * Restore the guest-owned EL1 system register image before returning to EL1.
 * Translation attributes and base registers are written before SCTLR_EL1 so
 * the MMU enable state observes a complete address-space definition. The local
 * TLB flush removes translations that may have been cached for the previous
 * vCPU that occupied this pCPU.
 */
static void restore_el1_sysregs(const struct arm64_vcpu_guest_ctx *gctx)
{
	write_ttbr0_el1(gctx->ttbr0_el1);
	write_ttbr1_el1(gctx->ttbr1_el1);
	write_tcr_el1(gctx->tcr_el1);
	write_mair_el1(gctx->mair_el1);
	write_amair_el1(gctx->amair_el1);
	write_vbar_el1(gctx->vbar_el1);
	write_contextidr_el1(gctx->contextidr_el1);
	write_cpacr_el1(gctx->cpacr_el1);
	write_tpidr_el0(gctx->tpidr_el0);
	write_tpidrro_el0(gctx->tpidrro_el0);
	write_tpidr_el1(gctx->tpidr_el1);
	write_sp_el0(gctx->sp_el0);
	write_elr_el1(gctx->elr_el1);
	write_spsr_el1(gctx->spsr_el1);
	write_esr_el1(gctx->esr_el1);
	write_far_el1(gctx->far_el1);
	write_afsr0_el1(gctx->afsr0_el1);
	write_afsr1_el1(gctx->afsr1_el1);
	write_par_el1(gctx->par_el1);
	write_cntkctl_el1(gctx->cntkctl_el1);
	write_sctlr_el1(gctx->sctlr_el1);
	flush_tlb_local();
}

/*
 * Snapshot all EL1 state that can affect guest execution after a context
 * switch. This is required for shared-pCPU scheduling: the hardware registers
 * are physically per-core, but the architectural state must follow the vCPU
 * thread as it is descheduled and later resumed.
 */
static void save_el1_sysregs(struct arm64_vcpu_guest_ctx *gctx)
{
	gctx->sctlr_el1 = read_sctlr_el1();
	gctx->ttbr0_el1 = read_ttbr0_el1();
	gctx->ttbr1_el1 = read_ttbr1_el1();
	gctx->tcr_el1 = read_tcr_el1();
	gctx->mair_el1 = read_mair_el1();
	gctx->amair_el1 = read_amair_el1();
	gctx->vbar_el1 = read_vbar_el1();
	gctx->contextidr_el1 = read_contextidr_el1();
	gctx->cpacr_el1 = read_cpacr_el1();
	gctx->tpidr_el0 = read_tpidr_el0();
	gctx->tpidrro_el0 = read_tpidrro_el0();
	gctx->tpidr_el1 = read_tpidr_el1();
	gctx->sp_el0 = read_sp_el0();
	gctx->elr_el1 = read_elr_el1();
	gctx->spsr_el1 = read_spsr_el1();
	gctx->esr_el1 = read_esr_el1();
	gctx->far_el1 = read_far_el1();
	gctx->afsr0_el1 = read_afsr0_el1();
	gctx->afsr1_el1 = read_afsr1_el1();
	gctx->par_el1 = read_par_el1();
	gctx->cntkctl_el1 = read_cntkctl_el1();
}

static void load_guest_timer(struct arm64_vcpu_guest_ctx *gctx)
{
	uint32_t ctl;

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

static void save_guest_timer(struct arm64_vcpu_guest_ctx *gctx)
{
	uint32_t ctl;
	uint32_t guest_ctl;
	uint64_t cval;
	uint64_t now;

	/*
	 * Linux may access the EL1 virtual timer directly when the trap is not
	 * taken by the CPU model. Keep the guest shadow synchronized on vCPU
	 * switch-out. EL2 may temporarily gate host PPI27 while a timer interrupt
	 * is in flight; keep that host mask out of the guest shadow.
	 */
	cval = read_cntv_cval_el0();
	ctl = read_cntv_ctl_el0() & (CNTV_CTL_ENABLE | CNTV_CTL_IMASK);
	guest_ctl = (gctx->timer_virq == ARM64_GIC_PPI_PHYSICAL_TIMER) ?
		gctx->cntp_ctl_el0 : gctx->cntv_ctl_el0;
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

void load_vcpu(__unused struct acrn_vcpu *vcpu)
{
	struct arm64_vcpu_guest_ctx *gctx = &vcpu->arch.gctx;

	/*
	 * VTTBR/VTCR select the VM's stage-2 table, VMPIDR gives the guest its
	 * virtual CPU identity, and CNTHCTL keeps guest timer programming trapped
	 * while allowing EL1 counter reads to stay direct. The host keeps the
	 * physical timer for scheduler ticks, while guest timer programming is
	 * carried in the vCPU's CNTV state and reconciled with the vGIC on writes.
	 *
	 * The EL1 register image and vGIC state must be loaded before guest entry
	 * so address translation, exception return state, and pending virtual
	 * interrupts are visible immediately after ERET.
	 */
	arm64_vgicv3_cancel_vtimer_backup(vcpu);
	write_vtcr_el2(gctx->vtcr_el2);
	write_vttbr_el2(gctx->vttbr_el2);
	flush_stage2_tlb_local();
	write_vmpidr_el2(vcpu_get_vmpidr(vcpu));
	write_cnthctl_el2(CNTHCTL_EL2_EL1PCTEN | CNTHCTL_EL2_EL1TVT);
	asm volatile ("msr cntvoff_el2, %0; isb" : : "r" (gctx->cntvoff_el2) : "memory");
	restore_el1_sysregs(gctx);
	load_guest_timer(gctx);
	arm64_vgicv3_load_vcpu(vcpu);
	arm64_vcpu_trace_vtimer_switch(vcpu, ARM64_VTIMER_TRACE_LOAD);
	write_hcr_el2(gctx->hcr_el2);
}

void unload_vcpu(__unused struct acrn_vcpu *vcpu)
{
	/*
	 * Save guest-owned EL1 state before clearing EL2 virtualization state.
	 * CNTV is disabled after its compare/control registers are captured so an
	 * expired deadline from this vCPU cannot interrupt the next vCPU scheduled
	 * on the same pCPU. Clearing HCR/VTTBR removes stale guest execution and
	 * stage-2 context before the host scheduler continues.
	 */
	struct arm64_vcpu_guest_ctx *gctx = &vcpu->arch.gctx;

	save_el1_sysregs(gctx);
	save_guest_timer(gctx);
	arm64_vgicv3_update_current_vtimer(vcpu);
	arm64_vcpu_trace_vtimer_switch(vcpu, ARM64_VTIMER_TRACE_UNLOAD);
	write_cntv_ctl_el0(0U);
	arm64_gicv3_enable_irq(ARM64_GIC_PPI_VIRTUAL_TIMER);
	arm64_gicv3_set_irq_priority(ARM64_GIC_PPI_VIRTUAL_TIMER,
		ARM64_GIC_PRIORITY_DEFAULT);
	arm64_vgicv3_save_vcpu(vcpu);
	arm64_vgicv3_arm_vtimer_backup(vcpu);
	write_hcr_el2(0UL);
	write_vttbr_el2(0UL);
	flush_stage2_tlb_local();
}

void flush_vcpu_context(__unused struct acrn_vcpu *vcpu)
{
}

bool is_vcpu_context_updated(__unused struct acrn_vcpu *vcpu)
{
	return false;
}

void vcpu_mark_context_dirty(__unused struct acrn_vcpu *vcpu)
{
}

int32_t arm64_process_vcpu_requests(struct acrn_vcpu *vcpu)
{
	if (vcpu_has_pending_request(vcpu)) {
		struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
		uint64_t flags;

		/*
		 * Requests are the cross-CPU handoff path for work that must be
		 * materialized before returning to EL1. Trap injection updates the
		 * saved guest frame; virtual IRQ injection syncs software IRQ state
		 * with the hardware list registers under the VM vGIC lock.
		 */
		if (vcpu_take_request(vcpu, ARM64_VCPU_REQUEST_EXCEPTION)) {
			vcpu_set_trap(vcpu, &vcpu->arch.trap);
			memset(&vcpu->arch.trap, 0, sizeof(struct arm64_vcpu_trap_info));
			vcpu->arch.trap.esr = EXCEPTION_INVALID;
		}

		spinlock_irqsave_obtain(&vgic->lock, &flags);
		arm64_vgicv3_sync_current_vcpu(vcpu);
		(void)vcpu_inject_pending_intr(vcpu);
		arm64_vgicv3_flush_current_vcpu(vcpu);
		spinlock_irqrestore_release(&vgic->lock, flags);
	}

	return 0;
}

int32_t arch_init_vcpu(struct acrn_vcpu *vcpu)
{
	/*
	 * Each vCPU points at the VM's stage-2 root and starts with EL1 AArch64
	 * enabled. HCR routes guest physical interrupts to EL2 and traps PSCI.
	 * WFI/WFE are left to the virtual CPU interface by default; trapping them
	 * on every guest idle/spin instruction is too expensive for the QEMU 3OS
	 * scenario and is only kept as a handler for future diagnostic modes.
	 */
	reset_vcpu(vcpu);
	return 0;
}

void arch_deinit_vcpu(__unused struct acrn_vcpu *vcpu)
{
	arm64_vgicv3_cancel_vtimer_backup(vcpu);
}

void arch_vcpu_thread(struct thread_object *obj)
{
	struct acrn_vcpu *vcpu = container_of(obj, struct acrn_vcpu, thread_obj);
	int32_t ret;

	/*
	 * The vCPU thread alternates between pre-entry request processing and the
	 * assembly guest-entry path. Exits return to the same thread stack, so the
	 * persistent register block is copied to/from a temporary trap frame rather
	 * than being used directly as the live EL2 stack.
	 */
	pr_info("vm%u:vcpu%u enter guest EL1 at 0x%lx",
		vcpu->vm->vm_id, vcpu->vcpu_id, vcpu->arch.regs.elr);

	while (vcpu->state == VCPU_RUNNING) {
		ret = arm64_process_vcpu_requests(vcpu);
		if (ret < 0) {
			break;
		}
		arm64_run_vcpu(&vcpu->arch);
	}

	vcpu_set_state(vcpu, VCPU_ZOMBIE);
	sleep_thread(obj);
	schedule();

	while (true) {
		cpu_relax();
	}
}

void arch_reset_vcpu(struct acrn_vcpu *vcpu)
{
	arm64_vgicv3_cancel_vtimer_backup(vcpu);
	memset(&vcpu->arch, 0, sizeof(vcpu->arch));
	arm64_prepare_linux_vcpu_context(vcpu, 0UL, 0UL);
}

void arch_context_switch_out(struct thread_object *prev)
{
	struct acrn_vcpu *vcpu = container_of(prev, struct acrn_vcpu, thread_obj);

	unload_vcpu(vcpu);
}

void arch_context_switch_in(struct thread_object *next)
{
	struct acrn_vcpu *vcpu = container_of(next, struct acrn_vcpu, thread_obj);

	load_vcpu(vcpu);
}

uint64_t arch_setup_thread_stack(struct thread_object *obj, uint8_t *stack, uint64_t stack_size)
{
	uint64_t stacktop = (uint64_t)&stack[stack_size];
	struct stack_frame *frame = (struct stack_frame *)stacktop;

	frame -= 1;
	memset(frame, 0, sizeof(struct stack_frame));

	frame->magic = SP_BOTTOM_MAGIC;
	frame->lr = (uint64_t)obj->thread_entry;
	frame->x0 = (uint64_t)obj;
	frame->spsr = read_daif();

	return (uint64_t)&frame->x19;
}

uint64_t arch_build_stack_frame(struct acrn_vcpu *vcpu)
{
	return arch_setup_thread_stack(&vcpu->thread_obj, vcpu->stack, CONFIG_STACK_SIZE);
}
