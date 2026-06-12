/*
 * Copyright (C) 2026 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <rtl.h>
#include <vcpu.h>
#include <vm.h>
#include <cpu.h>
#include <schedule.h>
#include <errno.h>
#include <logmsg.h>
#include <io_req.h>
#include <acrn_common.h>
#include <softirq.h>
#include <asm/platform.h>
#include <asm/cpu.h>
#include <asm/sysreg.h>
#include <asm/trap.h>
#include <asm/guest/vcpu_priv.h>
#include <asm/guest/vgicv3.h>

/*
 * Guest exits are entered from the EL2 vector table. Assembly saves the live
 * CPU state into a temporary struct cpu_regs frame on the vCPU thread stack;
 * this file converts that architectural exit state into common ACRN concepts
 * such as MMIO requests, PSCI CPU control, and virtual interrupt delivery.
 */
#define HPFAR_EL2_FIPA_MASK	0xfffffffff0UL
#define FAR_EL2_PAGE_MASK	0xfffUL

#define PSCI_0_2_FN_PSCI_VERSION	0x84000000U
#define PSCI_0_2_FN_CPU_OFF		0x84000002U
#define PSCI_0_2_FN_CPU_ON		0x84000003U
#define PSCI_0_2_FN_AFFINITY_INFO	0x84000004U
#define PSCI_0_2_FN_SYSTEM_OFF		0x84000008U
#define PSCI_0_2_FN_SYSTEM_RESET	0x84000009U
#define PSCI_0_2_FN64_CPU_ON		0xc4000003U
#define PSCI_0_2_FN64_AFFINITY_INFO	0xc4000004U

#define PSCI_RET_SUCCESS		0L
#define PSCI_RET_NOT_SUPPORTED		(-1L)
#define PSCI_RET_INVALID_PARAMS		(-2L)
#define PSCI_RET_DENIED			(-3L)
#define PSCI_AFFINITY_LEVEL_ON		0UL
#define PSCI_AFFINITY_LEVEL_OFF		1UL

#define ESR_SYSREG_DIR_READ		1UL
#define ESR_SYSREG_OP0_SHIFT		20U
#define ESR_SYSREG_OP0_MASK		0x3UL
#define ESR_SYSREG_OP2_SHIFT		17U
#define ESR_SYSREG_OP2_MASK		0x7UL
#define ESR_SYSREG_OP1_SHIFT		14U
#define ESR_SYSREG_OP1_MASK		0x7UL
#define ESR_SYSREG_CRN_SHIFT		10U
#define ESR_SYSREG_CRN_MASK		0xfUL
#define ESR_SYSREG_RT_SHIFT		5U
#define ESR_SYSREG_RT_MASK		0x1fUL
#define ESR_SYSREG_CRM_SHIFT		1U
#define ESR_SYSREG_CRM_MASK		0xfUL

#define SYSREG_ENC(op0, op1, crn, crm, op2) \
	(((op0) << 20U) | ((op2) << 17U) | ((op1) << 14U) | ((crn) << 10U) | ((crm) << 1U))
#define SYSREG_ICC_SGI1R_EL1		SYSREG_ENC(3UL, 0UL, 12UL, 11UL, 5UL)
#define SYSREG_CNTP_CTL_EL0		SYSREG_ENC(3UL, 3UL, 14UL, 2UL, 1UL)
#define SYSREG_CNTP_CVAL_EL0		SYSREG_ENC(3UL, 3UL, 14UL, 2UL, 2UL)
#define SYSREG_CNTP_TVAL_EL0		SYSREG_ENC(3UL, 3UL, 14UL, 2UL, 0UL)
#define SYSREG_CNTPCT_EL0		SYSREG_ENC(3UL, 3UL, 14UL, 0UL, 1UL)
#define SYSREG_CNTV_CTL_EL0		SYSREG_ENC(3UL, 3UL, 14UL, 3UL, 1UL)
#define SYSREG_CNTV_CVAL_EL0		SYSREG_ENC(3UL, 3UL, 14UL, 3UL, 2UL)
#define SYSREG_CNTV_TVAL_EL0		SYSREG_ENC(3UL, 3UL, 14UL, 3UL, 0UL)
#define SYSREG_CNTVCT_EL0		SYSREG_ENC(3UL, 3UL, 14UL, 0UL, 2UL)

static uint64_t arm64_fault_ipa(const struct cpu_regs *regs)
{
	/*
	 * For a stage-2 data abort, HPFAR_EL2 contains the faulting IPA page
	 * number and FAR_EL2 provides the byte offset inside that page.
	 */
	return ((regs->hpfar & HPFAR_EL2_FIPA_MASK) << 8U) |
		(regs->far & FAR_EL2_PAGE_MASK);
}

static struct acrn_vcpu *get_exit_vcpu(uint16_t pcpu_id)
{
	struct acrn_vcpu *vcpu = get_running_vcpu(pcpu_id);

	if (vcpu == NULL) {
		pr_fatal("arm64 vcpu exit without current vcpu on pcpu%hu", pcpu_id);
		cpu_dead();
	}

	return vcpu;
}

static void save_exit_regs(struct acrn_vcpu *vcpu, const struct cpu_regs *regs)
{
	/*
	 * Keep the persistent guest image in vcpu->arch.regs. The vector stack
	 * frame is temporary and may be rebuilt after scheduling or softirq work.
	 */
	(void)memcpy_s(&vcpu->arch.regs, sizeof(vcpu->arch.regs),
		regs, sizeof(*regs));
}

static void restore_exit_regs(struct cpu_regs *regs, const struct acrn_vcpu *vcpu)
{
	(void)memcpy_s(regs, sizeof(*regs),
		&vcpu->arch.regs, sizeof(vcpu->arch.regs));
}

static uint64_t *arm64_gpr(struct cpu_regs *regs, uint32_t idx)
{
	return (idx < 31U) ? (&regs->x0 + idx) : NULL;
}

static uint64_t mmio_size_mask(uint64_t size)
{
	return (size >= sizeof(uint64_t)) ? ~0UL : ((1UL << (size * 8U)) - 1UL);
}

static uint64_t extend_mmio_read(uint64_t value, uint64_t size, uint64_t esr)
{
	uint64_t mask = mmio_size_mask(size);

	value &= mask;
	if (((esr & ESR_DABT_SSE) != 0UL) && (size < sizeof(uint64_t))) {
		uint64_t sign = 1UL << ((size * 8U) - 1U);

		if ((value & sign) != 0UL) {
			value |= ~mask;
		}
	}
	if ((esr & ESR_DABT_SF) == 0UL) {
		value &= 0xffffffffUL;
	}

	return value;
}

static void advance_vcpu_elr(struct acrn_vcpu *vcpu)
{
	struct cpu_regs *regs = &vcpu->arch.regs;

	regs->elr += ((regs->esr & ESR_EL2_IL) != 0UL) ? 4UL : 2UL;
}

static int32_t handle_mmio_abort(struct acrn_vcpu *vcpu)
{
	struct cpu_regs *regs = &vcpu->arch.regs;
	struct io_request *io_req = &vcpu->req;
	struct acrn_mmio_request *mmio = &io_req->reqs.mmio_request;
	uint64_t esr = regs->esr;
	uint64_t ipa;
	uint64_t size;
	uint32_t reg_idx;
	uint64_t *reg;
	int32_t ret;

	/*
	 * Unmapped stage-2 device windows intentionally raise data-abort exits.
	 * When ESR.ISV is set, the syndrome gives the access width, direction, and
	 * source/target GPR so the generic MMIO emulator can complete the access
	 * without decoding the original instruction bytes.
	 */
	if ((esr & ESR_DABT_ISV) == 0UL) {
		return -EINVAL;
	}

	ipa = arm64_fault_ipa(regs);
	size = 1UL << ((esr >> ESR_DABT_SAS_SHIFT) & ESR_DABT_SAS_MASK);
	reg_idx = (uint32_t)((esr >> ESR_DABT_SRT_SHIFT) & ESR_DABT_SRT_MASK);
	reg = arm64_gpr(regs, reg_idx);

	(void)memset(io_req, 0U, sizeof(*io_req));
	io_req->io_type = ACRN_IOREQ_TYPE_MMIO;
	mmio->address = ipa;
	mmio->size = size;
	if ((esr & ESR_DABT_WNR) != 0UL) {
		mmio->direction = ACRN_IOREQ_DIR_WRITE;
		mmio->value = (reg != NULL) ? (*reg & mmio_size_mask(size)) : 0UL;
	} else {
		mmio->direction = ACRN_IOREQ_DIR_READ;
	}

	ret = emulate_io(vcpu, io_req);
	if (ret == 0) {
		if (((esr & ESR_DABT_WNR) == 0UL) && (reg != NULL)) {
			*reg = extend_mmio_read(mmio->value, size, esr);
		}
		advance_vcpu_elr(vcpu);
	}

	return ret;
}

static struct acrn_vcpu *psci_target_vcpu(struct acrn_vm *vm, uint64_t mpidr)
{
	uint16_t idx;
	struct acrn_vcpu *vcpu;

	foreach_vcpu(idx, vm, vcpu) {
		if (vcpu_get_vmpidr(vcpu) == (mpidr & MPIDR_AFFINITY_MASK)) {
			return vcpu;
		}
	}

	return NULL;
}

static int64_t handle_psci_cpu_on(struct acrn_vcpu *vcpu)
{
	struct acrn_vcpu *target = psci_target_vcpu(vcpu->vm, vcpu->arch.regs.x1);
	int64_t ret = PSCI_RET_INVALID_PARAMS;

	if (target != NULL) {
		if (target->state == VCPU_RUNNING) {
			ret = PSCI_RET_DENIED;
		} else {
			target->arch.regs.elr = vcpu->arch.regs.x2;
			target->arch.regs.x0 = vcpu->arch.regs.x3;
			target->arch.regs.spsr = (vcpu->arch.regs.spsr & DAIF_ALL) | 0x5UL;
			launch_vcpu(target);
			ret = PSCI_RET_SUCCESS;
		}
	}

	return ret;
}

static int32_t handle_psci64(struct acrn_vcpu *vcpu, bool advance_elr)
{
	uint32_t fn = (uint32_t)vcpu->arch.regs.x0;
	struct acrn_vcpu *target;
	int64_t ret;

	/*
	 * PSCI is the guest-visible CPU power-management ABI. The current QEMU
	 * model handles the calls needed to boot and stop vCPUs locally instead of
	 * forwarding SMC/HVC requests to firmware.
	 */
	switch (fn) {
	case PSCI_0_2_FN_PSCI_VERSION:
		ret = 0x00010000L;
		break;
	case PSCI_0_2_FN_CPU_ON:
	case PSCI_0_2_FN64_CPU_ON:
		ret = handle_psci_cpu_on(vcpu);
		break;
	case PSCI_0_2_FN_CPU_OFF:
		zombie_vcpu(vcpu);
		ret = PSCI_RET_SUCCESS;
		break;
	case PSCI_0_2_FN_AFFINITY_INFO:
	case PSCI_0_2_FN64_AFFINITY_INFO:
		target = psci_target_vcpu(vcpu->vm, vcpu->arch.regs.x1);
		ret = ((target != NULL) && (target->state == VCPU_RUNNING)) ?
			PSCI_AFFINITY_LEVEL_ON : PSCI_AFFINITY_LEVEL_OFF;
		break;
	case PSCI_0_2_FN_SYSTEM_OFF:
	case PSCI_0_2_FN_SYSTEM_RESET:
		pr_info("vm%u psci system request 0x%x", vcpu->vm->vm_id, fn);
		zombie_vcpu(vcpu);
		ret = PSCI_RET_SUCCESS;
		break;
	default:
		pr_warn("vm%u-vcpu%u unsupported psci call 0x%x",
			vcpu->vm->vm_id, vcpu->vcpu_id, fn);
		ret = PSCI_RET_NOT_SUPPORTED;
		break;
	}

	vcpu->arch.regs.x0 = (uint64_t)ret;
	if (advance_elr) {
		advance_vcpu_elr(vcpu);
	}
	return 0;
}

static int32_t handle_hvc64(struct acrn_vcpu *vcpu)
{
	int32_t ret;

	if (arm64_is_acrn_hypercall(vcpu->arch.regs.x0)) {
		ret = arm64_dispatch_hypercall(vcpu);
	} else {
		ret = handle_psci64(vcpu, false);
	}

	return ret;
}

/*
 * The host owns CNTP for the per-pCPU scheduler tick, so guest accesses to
 * either the physical-timer or virtual-timer ABI are backed by the vCPU's CNTV
 * state. timer_virq remembers which PPI number the guest expects: RTOS guests
 * that use CNTP still receive the physical-timer PPI, while guests using CNTV
 * receive the virtual-timer PPI. This keeps the host deadline independent from
 * guest timer programming while preserving the guest-visible interrupt ABI.
 */
static int32_t handle_timer_sysreg(struct acrn_vcpu *vcpu, uint64_t sysreg, bool read, uint64_t *reg)
{
	struct arm64_vcpu_guest_ctx *gctx = &vcpu->arch.gctx;
	uint64_t now = read_cntvct_el0();
	uint64_t val;
	int32_t ret = 0;

	if (reg == NULL) {
		return -EINVAL;
	}

	switch (sysreg) {
	case SYSREG_CNTPCT_EL0:
		gctx->timer_virq = ARM64_GIC_PPI_PHYSICAL_TIMER;
		if (read) {
			*reg = read_cntvct_el0();
		} else {
			ret = -EINVAL;
		}
		break;
	case SYSREG_CNTVCT_EL0:
		gctx->timer_virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
		if (read) {
			*reg = read_cntvct_el0();
		} else {
			ret = -EINVAL;
		}
		break;
	case SYSREG_CNTP_CTL_EL0:
		gctx->timer_virq = ARM64_GIC_PPI_PHYSICAL_TIMER;
		if (read) {
			*reg = read_cntv_ctl_el0();
		} else {
			gctx->cntv_ctl_el0 = (uint32_t)*reg;
			write_cntv_ctl_el0(gctx->cntv_ctl_el0);
		}
		break;
	case SYSREG_CNTV_CTL_EL0:
		gctx->timer_virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
		if (read) {
			*reg = read_cntv_ctl_el0();
		} else {
			gctx->cntv_ctl_el0 = (uint32_t)*reg;
			write_cntv_ctl_el0(gctx->cntv_ctl_el0);
		}
		break;
	case SYSREG_CNTP_CVAL_EL0:
		gctx->timer_virq = ARM64_GIC_PPI_PHYSICAL_TIMER;
		if (read) {
			*reg = read_cntv_cval_el0();
		} else {
			gctx->cntv_cval_el0 = *reg;
			write_cntv_cval_el0(gctx->cntv_cval_el0);
		}
		break;
	case SYSREG_CNTV_CVAL_EL0:
		gctx->timer_virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
		if (read) {
			*reg = read_cntv_cval_el0();
		} else {
			gctx->cntv_cval_el0 = *reg;
			write_cntv_cval_el0(gctx->cntv_cval_el0);
		}
		break;
	case SYSREG_CNTP_TVAL_EL0:
		gctx->timer_virq = ARM64_GIC_PPI_PHYSICAL_TIMER;
		if (read) {
			*reg = read_cntv_cval_el0() - now;
		} else {
			val = now + (uint32_t)*reg;
			gctx->cntv_cval_el0 = val;
			write_cntv_cval_el0(val);
		}
		break;
	case SYSREG_CNTV_TVAL_EL0:
		gctx->timer_virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
		if (read) {
			*reg = read_cntv_cval_el0() - now;
		} else {
			val = now + (uint32_t)*reg;
			gctx->cntv_cval_el0 = val;
			write_cntv_cval_el0(val);
		}
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int32_t handle_sysreg(struct acrn_vcpu *vcpu)
{
	struct cpu_regs *regs = &vcpu->arch.regs;
	uint64_t esr = regs->esr;
	uint64_t iss = esr & ((1UL << 25U) - 1UL);
	uint64_t sysreg = iss & ~(ESR_SYSREG_RT_MASK << ESR_SYSREG_RT_SHIFT) &
		~ESR_SYSREG_DIR_READ;
	uint32_t rt = (uint32_t)((iss >> ESR_SYSREG_RT_SHIFT) & ESR_SYSREG_RT_MASK);
	uint64_t *reg = arm64_gpr(regs, rt);
	int32_t ret = -EINVAL;

	/*
	 * ICC_SGI1R_EL1 is trapped so guest SGI sends become vGIC software state
	 * updates. Other system registers are left unsupported until a guest needs
	 * them; failing closed makes missing virtualization explicit.
	 */
	if (((iss & ESR_SYSREG_DIR_READ) == 0UL) && (sysreg == SYSREG_ICC_SGI1R_EL1) &&
		(reg != NULL)) {
		ret = arm64_vgicv3_handle_sgi1r(vcpu, *reg);
		if (ret == 0) {
			advance_vcpu_elr(vcpu);
		}
	} else if ((sysreg == SYSREG_CNTPCT_EL0) || (sysreg == SYSREG_CNTVCT_EL0) ||
		(sysreg == SYSREG_CNTP_CTL_EL0) || (sysreg == SYSREG_CNTP_CVAL_EL0) ||
		(sysreg == SYSREG_CNTP_TVAL_EL0) || (sysreg == SYSREG_CNTV_CTL_EL0) ||
		(sysreg == SYSREG_CNTV_CVAL_EL0) || (sysreg == SYSREG_CNTV_TVAL_EL0)) {
		ret = handle_timer_sysreg(vcpu, sysreg, ((iss & ESR_SYSREG_DIR_READ) != 0UL), reg);
		if (ret == 0) {
			advance_vcpu_elr(vcpu);
		}
	}

	return ret;
}

int32_t vcpu_exit_handler(struct acrn_vcpu *vcpu)
{
	uint64_t ec = ESR_EL2_EC(vcpu->arch.regs.esr);
	int32_t ret = -EINVAL;

	/*
	 * ESR.EC identifies the architectural reason for the EL1-to-EL2 exit.
	 * Each handled class must either update the saved guest frame and advance
	 * ELR, or deliberately leave the vCPU stopped on failure.
	 */
	switch (ec) {
	case ESR_EL2_EC_DABT_LOW:
		ret = handle_mmio_abort(vcpu);
		break;
	case ESR_EL2_EC_HVC64:
		ret = handle_hvc64(vcpu);
		break;
	case ESR_EL2_EC_SMC64:
		ret = handle_psci64(vcpu, true);
		break;
	case ESR_EL2_EC_SYSREG:
		ret = handle_sysreg(vcpu);
		break;
	case ESR_EL2_EC_WFI_WFE:
		advance_vcpu_elr(vcpu);
		ret = 0;
		break;
	default:
		break;
	}

	if (ret == 0) {
		return 0;
	}

	pr_err("unhandled arm64 vcpu exit esr=0x%lx elr=0x%lx",
		vcpu->arch.regs.esr, vcpu->arch.regs.elr);
	return -EINVAL;
}

void dispatch_vcpu_trap(struct cpu_regs *regs)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct acrn_vcpu *vcpu = get_exit_vcpu(pcpu_id);
	int32_t ret;

	/*
	 * Synchronous guest exits are handled on the current vCPU thread. The saved
	 * frame may be modified by emulation, scheduling, or request processing
	 * before being restored to the assembly return path.
	 */
	save_exit_regs(vcpu, regs);

	ret = vcpu_exit_handler(vcpu);
	if (ret < 0) {
		pr_err("failed to handle arm64 vcpu exit. ret=%d", ret);
		get_vm_lock(vcpu->vm);
		zombie_vcpu(vcpu);
		put_vm_lock(vcpu->vm);
	}

	local_irq_disable();

	if (need_reschedule(pcpu_id)) {
		schedule();
	}

	ret = arm64_process_vcpu_requests(vcpu);
	if (ret < 0) {
		pr_fatal("failed to process arm64 vcpu requests");
		get_vm_lock(vcpu->vm);
		zombie_vcpu(vcpu);
		put_vm_lock(vcpu->vm);
		schedule();
	}

	restore_exit_regs(regs, vcpu);
}

void dispatch_vcpu_irq(struct cpu_regs *regs)
{
	uint16_t pcpu_id = get_pcpu_id();
	struct acrn_vcpu *vcpu = get_exit_vcpu(pcpu_id);
	int32_t ret;

	/*
	 * Physical IRQs taken while a guest is running are handled without enabling
	 * nested softirq processing in the low-level IRQ path. Softirqs run once
	 * the interrupt is acknowledged. If the timer tick made another vCPU
	 * runnable, schedule before restoring the trap frame; otherwise a vCPU on a
	 * shared pCPU can immediately re-enter EL1 and starve the peer vCPU that is
	 * waiting for its time slice. Virtual interrupt state is resynced after the
	 * possible context switch and before returning to EL1.
	 */
	save_exit_regs(vcpu, regs);

	dispatch_interrupt_no_softirq((const struct intr_excp_ctx *)regs);
	local_irq_disable();
	do_softirq_no_irqenable();

	if (need_reschedule(pcpu_id)) {
		schedule();
	}

	ret = arm64_process_vcpu_requests(vcpu);
	if (ret < 0) {
		pr_fatal("failed to process arm64 vcpu requests");
		get_vm_lock(vcpu->vm);
		zombie_vcpu(vcpu);
		put_vm_lock(vcpu->vm);
		schedule();
	}

	restore_exit_regs(regs, vcpu);
}
