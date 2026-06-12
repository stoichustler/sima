/*
 * Copyright (C) 2026 Intel Corporation.
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
#include <asm/irq.h>
#include <asm/cpu.h>
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

void vcpu_set_elr(struct acrn_vcpu *vcpu, uint64_t val)
{
	vcpu->arch.regs.elr = val;
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
}

void load_vcpu(__unused struct acrn_vcpu *vcpu)
{
	struct arm64_vcpu_guest_ctx *gctx = &vcpu->arch.gctx;

	/*
	 * VTTBR/VTCR select the VM's stage-2 table, VMPIDR gives the guest its
	 * virtual CPU identity, and CNTHCTL exposes only the virtual counter/timer
	 * to EL1. The host keeps the physical timer for scheduler ticks, while
	 * guest timer programming is carried in the vCPU's CNTV state.
	 *
	 * The EL1 register image and vGIC state must be loaded before guest entry
	 * so address translation, exception return state, and pending virtual
	 * interrupts are visible immediately after ERET.
	 */
	write_vtcr_el2(gctx->vtcr_el2);
	write_vttbr_el2(gctx->vttbr_el2);
	flush_stage2_tlb_local();
	write_vmpidr_el2(vcpu_get_vmpidr(vcpu));
	write_cnthctl_el2(CNTHCTL_EL2_EL1VCTEN | CNTHCTL_EL2_EL1VTEN);
	asm volatile ("msr cntvoff_el2, %0; isb" : : "r" (gctx->cntvoff_el2) : "memory");
	restore_el1_sysregs(gctx);
	write_cntv_cval_el0(gctx->cntv_cval_el0);
	write_cntv_ctl_el0(gctx->cntv_ctl_el0);
	arm64_vgicv3_load_vcpu(vcpu);
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
	gctx->cntv_cval_el0 = read_cntv_cval_el0();
	gctx->cntv_ctl_el0 = read_cntv_ctl_el0();
	write_cntv_ctl_el0(0U);
	arm64_vgicv3_save_vcpu(vcpu);
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
	struct arm64_vcpu_guest_ctx *gctx = &vcpu->arch.gctx;

	/*
	 * Each vCPU points at the VM's stage-2 root and starts with EL1 AArch64
	 * enabled. Traps for PSCI and IRQ routing are configured through HCR, while
	 * CNTVOFF gives the guest a VM-relative virtual counter.
	 */
	reset_vcpu(vcpu);
	gctx->vttbr_el2 = hva2hpa(vcpu->vm->root_stg2ptp);
	gctx->vtcr_el2 = VTCR_EL2_VALUE;
	gctx->hcr_el2 = HCR_VM | HCR_RW | HCR_IMO | HCR_FMO | HCR_AMO | HCR_TSC;
	gctx->cntvoff_el2 = (uint64_t)vcpu->vm->arch_vm.time_delta;
	gctx->cntv_cval_el0 = 0UL;
	gctx->cntv_ctl_el0 = 0U;
	gctx->timer_virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
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
	arm64_vgicv3_init_vcpu(vcpu);
	return 0;
}

void arch_deinit_vcpu(__unused struct acrn_vcpu *vcpu)
{
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
	pr_info("vm%u-vcpu%u enter guest el1 at 0x%lx",
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
	memset(&vcpu->arch, 0, sizeof(vcpu->arch));
	vcpu->arch.trap.esr = EXCEPTION_INVALID;
	vcpu->arch.regs.spsr = SPSR_EL2_MODE_EL1H | DAIF_ALL;
	arm64_vgicv3_reset_vcpu(vcpu);
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
