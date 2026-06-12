/*
 * Copyright (C) 2026 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <bits.h>
#include <vm.h>
#include <vcpu.h>
#include <io_req.h>
#include <irq.h>
#include <logmsg.h>
#include <rtl.h>
#include <asm/platform.h>
#include <asm/sysreg.h>
#include <asm/guest/vgicv3.h>

/*
 * ARM64 GICv3 virtualization has a split model:
 * - hardware list registers (LRs) are loaded for the currently running vCPU,
 * - software descriptors keep the guest-visible pending/active/enable state,
 * - guest MMIO/system-register accesses mutate the software model.
 *
 * Sync pulls completed LR state back into software. Flush pushes pending
 * software state into available LRs. This keeps the common scheduler free to
 * switch vCPUs without losing in-flight virtual interrupt state.
 */
#define BIT32(n)			(1U << (n))

#define VGICD_CTLR			0x0000U
#define VGICD_TYPER			0x0004U
#define VGICD_IIDR			0x0008U
#define VGICD_IGROUPR			0x0080U
#define VGICD_ISENABLER		0x0100U
#define VGICD_ICENABLER		0x0180U
#define VGICD_ISPENDR			0x0200U
#define VGICD_ICPENDR			0x0280U
#define VGICD_ISACTIVER		0x0300U
#define VGICD_ICACTIVER		0x0380U
#define VGICD_IPRIORITYR		0x0400U
#define VGICD_ICFGR			0x0C00U
#define VGICD_IROUTER			0x6000U
#define VGICD_PIDR2			0xFFE8U

#define VGICR_CTLR			0x0000U
#define VGICR_IIDR			0x0004U
#define VGICR_TYPER			0x0008U
#define VGICR_WAKER			0x0014U
#define VGICR_PIDR2			0xFFE8U
#define VGICR_SGI_BASE			0x10000U
#define VGICR_SGI_END			(VGICR_SGI_BASE + 0x10000U)

#define VGIC_CTLR_ENABLE_G1		BIT32(1U)
#define VGIC_CTLR_ARE_NS		BIT32(5U)
#define VGICR_TYPER_LAST		(1UL << 4U)
#define VGICR_TYPER_PROCNUM_SHIFT	8U
#define VGICR_TYPER_AFF_SHIFT		32U
#define VGICR_WAKER_PROCESSOR_SLEEP	BIT32(1U)
#define VGICR_WAKER_CHILDREN_ASLEEP	BIT32(2U)

#define ICC_SGI1R_TARGET_LIST_MASK	0xffffUL
#define ICC_SGI1R_AFF1_SHIFT		16U
#define ICC_SGI1R_INTID_SHIFT		24U
#define ICC_SGI1R_INTID_MASK		0xfUL
#define ICC_SGI1R_IRM			(1UL << 40U)
#define ICC_SGI1R_AFF2_SHIFT		32U
#define ICC_SGI1R_AFF3_SHIFT		48U
#define ICC_SGI1R_AFF_MASK		0xffUL

#define GIC_PRIORITY_DEFAULT		0x80U
#define GIC_PRIORITY_LOWEST		0xffU
#define GIC_PIDR2_ARCH_GICV3		(3U << 4U)

static uint32_t vgic_lr_count;
static bool vgic_global_initialized;

static uint32_t min_u32(uint32_t a, uint32_t b)
{
	return (a < b) ? a : b;
}

static bool addr_in_range(uint64_t addr, uint64_t base, uint64_t size)
{
	return (addr >= base) && (addr < (base + size));
}

static struct arm64_vgic_irq *vgic_irq_desc(struct acrn_vcpu *vcpu, uint32_t virq)
{
	struct arm64_vgic_irq *desc = NULL;
	struct arm64_vgicv3 *vgic;

	if ((vcpu != NULL) && (vcpu->vm != NULL) && (virq < ARM64_VGIC_IRQ_NUM) &&
		(vcpu->vcpu_id < ARM64_VGIC_MAX_VCPUS)) {
		vgic = &vcpu->vm->arch_vm.vgic;
		if (vgic->initialized) {
			desc = &vgic->irq[vcpu->vcpu_id][virq];
		}
	}

	return desc;
}

static uint64_t make_lr(const struct arm64_vgic_irq *desc)
{
	uint64_t lr = desc->virq & ICH_LR_VINTID_MASK;
	uint64_t state = ICH_LR_STATE_PENDING;

	/*
	 * The LR state encodes the guest-visible lifecycle. Edge interrupts are
	 * pending-only until accepted; level interrupts become active after being
	 * presented so the maintenance interrupt can later reconcile completion.
	 */
	if (desc->active) {
		state = desc->pending ? ICH_LR_STATE_ACTIVE_PENDING : ICH_LR_STATE_ACTIVE;
	}

	lr |= ((uint64_t)desc->priority << ICH_LR_PRIORITY_SHIFT);
	lr |= ICH_LR_GROUP1;
	lr |= state << ICH_LR_STATE_SHIFT;

	if (desc->hw) {
		lr |= ((uint64_t)desc->pirq << ICH_LR_PINTID_SHIFT);
		lr |= ICH_LR_HW;
	}

	return lr;
}

static uint32_t lr_vintid(uint64_t lr)
{
	return (uint32_t)(lr & ICH_LR_VINTID_MASK);
}

static uint32_t lr_state(uint64_t lr)
{
	return (uint32_t)((lr >> ICH_LR_STATE_SHIFT) & 0x3UL);
}

static int32_t find_lr_for_virq(const struct acrn_vcpu *vcpu, uint32_t virq)
{
	uint32_t idx;

	for (idx = 0U; idx < vcpu->arch.vgic.used_lrs; idx++) {
		if (lr_vintid(vcpu->arch.vgic.lr[idx]) == virq) {
			return (int32_t)idx;
		}
	}

	return -1;
}

static void remove_lr(struct acrn_vcpu *vcpu, uint32_t idx)
{
	struct arm64_vgicv3_vcpu_ctx *ctx = &vcpu->arch.vgic;
	uint32_t last;

	if (idx < ctx->used_lrs) {
		last = ctx->used_lrs - 1U;
		if (idx != last) {
			ctx->lr[idx] = ctx->lr[last];
		}
		ctx->lr[last] = 0UL;
		ctx->used_lrs--;
	}
}

static void vgicv3_write_lrs(const struct arm64_vgicv3_vcpu_ctx *ctx)
{
	uint32_t idx;
	uint32_t count = min_u32(vgic_lr_count, ARM64_VGIC_MAX_LRS);

	for (idx = 0U; idx < count; idx++) {
		write_ich_lr_el2((uint8_t)idx, (idx < ctx->used_lrs) ? ctx->lr[idx] : 0UL);
	}
}

static void vgicv3_read_lrs(struct arm64_vgicv3_vcpu_ctx *ctx)
{
	uint32_t idx;
	uint32_t count = min_u32(vgic_lr_count, ARM64_VGIC_MAX_LRS);

	/*
	 * Hardware may clear an LR after the guest acknowledges/deactivates an
	 * interrupt. Keep used_lrs as the highest non-empty slot so later code can
	 * compact the software copy without scanning unused hardware capacity.
	 */
	ctx->used_lrs = 0U;
	for (idx = 0U; idx < count; idx++) {
		ctx->lr[idx] = read_ich_lr_el2((uint8_t)idx);
		if (lr_state(ctx->lr[idx]) != ICH_LR_STATE_INVALID) {
			ctx->used_lrs = (uint8_t)(idx + 1U);
		}
	}
}

static bool vgicv3_vcpu_is_loaded(const struct acrn_vcpu *vcpu)
{
	return get_running_vcpu(get_pcpu_id()) == vcpu;
}

static void vgicv3_read_loaded_lrs(struct acrn_vcpu *vcpu, bool is_current)
{
	if (is_current || vgicv3_vcpu_is_loaded(vcpu)) {
		vgicv3_read_lrs(&vcpu->arch.vgic);
	}
}

static void vgicv3_write_loaded_lrs(const struct acrn_vcpu *vcpu, bool is_current)
{
	if (is_current || vgicv3_vcpu_is_loaded(vcpu)) {
		vgicv3_write_lrs(&vcpu->arch.vgic);
		write_ich_hcr_el2(vcpu->arch.vgic.hcr | ICH_HCR_EN);
	}
}

void arm64_vgicv3_global_init(void)
{
	if (!vgic_global_initialized) {
		/*
		 * ICH_VTR_EL2 reports the hardware LR count minus one. Clamp to
		 * the static context array size so the save/restore path has a
		 * fixed bound even on larger GIC implementations.
		 */
		vgic_lr_count = (uint32_t)((read_ich_vtr_el2() & 0x1fUL) + 1UL);
		if (vgic_lr_count > ARM64_VGIC_MAX_LRS) {
			vgic_lr_count = ARM64_VGIC_MAX_LRS;
		}
		if (vgic_lr_count == 0U) {
			panic("gicv3 virtualization has no list registers");
		}
		vgic_global_initialized = true;
		pr_info("gicv3 virtualization: %u list registers", vgic_lr_count);
	}
}

void arm64_vgicv3_init_vm(struct acrn_vm *vm)
{
	struct arm64_vgicv3 *vgic = &vm->arch_vm.vgic;
	uint32_t idx;

	/*
	 * The virtual distributor advertises a compact GICv3 with affinity routing
	 * and Group-1 interrupts enabled. Each vCPU owns a row of IRQ descriptors
	 * so SGIs/PPIs are naturally per-vCPU in the software model.
	 */
	arm64_vgicv3_global_init();

	(void)memset(vgic, 0U, sizeof(*vgic));
	spinlock_init(&vgic->lock);
	vgic->lr_count = vgic_lr_count;
	vgic->vmcr = (uint32_t)(ICH_VMCR_VENG1 | ICH_VMCR_DEFAULT_MASK);
	vgic->gicd_ctlr = VGIC_CTLR_ARE_NS | VGIC_CTLR_ENABLE_G1;
	vgic->gicd_typer = (((ARM64_VGIC_IRQ_NUM / 32U) - 1U) & 0x1fU) |
		((ARM64_VGIC_MAX_VCPUS & 0x7U) << 5U);
	vgic->gicd_pidr2 = GIC_PIDR2_ARCH_GICV3;

	for (idx = 0U; idx < ARM64_VGIC_MAX_VCPUS; idx++) {
		uint32_t virq;

		for (virq = 0U; virq < ARM64_VGIC_IRQ_NUM; virq++) {
			struct arm64_vgic_irq *desc = &vgic->irq[idx][virq];

			desc->virq = (uint16_t)virq;
			desc->pirq = 0xffffU;
			desc->priority = GIC_PRIORITY_DEFAULT;
			desc->target_vcpu = (uint8_t)idx;
			desc->level = (virq >= ARM64_VGIC_LOCAL_IRQ_NUM);
		}
	}

	vgic->initialized = true;
}

void arm64_vgicv3_init_vcpu(struct acrn_vcpu *vcpu)
{
	struct arm64_vgicv3_vcpu_ctx *ctx = &vcpu->arch.vgic;

	(void)memset(ctx, 0U, sizeof(*ctx));
	ctx->hcr = ICH_HCR_EN;
	ctx->vmcr = ICH_VMCR_VENG1 | ICH_VMCR_DEFAULT_MASK;
	ctx->sre = ICC_SRE_SRE;
	ctx->pmr = GIC_PRIORITY_LOWEST;
	ctx->ctlr = 0UL;
}

void arm64_vgicv3_reset_vcpu(struct acrn_vcpu *vcpu)
{
	arm64_vgicv3_init_vcpu(vcpu);
}

void arm64_vgicv3_load_vcpu(struct acrn_vcpu *vcpu)
{
	struct arm64_vgicv3_vcpu_ctx *ctx = &vcpu->arch.vgic;

	/*
	 * Program the EL1-visible GIC CPU interface and then enable the virtual
	 * control interface. LRs must be written before ICH_HCR.EN so pending
	 * virtual interrupts are observable as soon as the guest resumes.
	 */
	write_icc_sre_el1(ctx->sre | ICC_SRE_SRE);
	write_ich_vmcr_el2(ctx->vmcr);
	write_ich_ap0r0_el2(ctx->ap0r0);
	write_ich_ap1r0_el2(ctx->ap1r0);
	vgicv3_write_lrs(ctx);
	write_ich_hcr_el2(ctx->hcr | ICH_HCR_EN);
}

void arm64_vgicv3_save_vcpu(struct acrn_vcpu *vcpu)
{
	struct arm64_vgicv3_vcpu_ctx *ctx = &vcpu->arch.vgic;

	ctx->hcr = read_ich_hcr_el2();
	ctx->vmcr = read_ich_vmcr_el2();
	ctx->ap0r0 = read_ich_ap0r0_el2();
	ctx->ap1r0 = read_ich_ap1r0_el2();
	ctx->sre = read_icc_sre_el1();
	ctx->pmr = read_icc_pmr_el1();
	ctx->ctlr = read_icc_ctlr_el1();
	vgicv3_read_lrs(ctx);
	write_ich_hcr_el2(0UL);
}

static void vgicv3_sync_vcpu(struct acrn_vcpu *vcpu, bool is_current)
{
	struct arm64_vgicv3_vcpu_ctx *ctx = &vcpu->arch.vgic;
	uint32_t idx = 0U;

	/*
	 * Synchronization is the authoritative readback point from hardware LRs
	 * into the software IRQ descriptors. It is called before changing virtual
	 * IRQ state and from maintenance IRQs when hardware reports LR pressure or
	 * completion.
	 */
	vgicv3_read_loaded_lrs(vcpu, is_current);

	while (idx < ctx->used_lrs) {
		uint64_t lr = ctx->lr[idx];
		uint32_t virq = lr_vintid(lr);
		uint32_t state = lr_state(lr);
		struct arm64_vgic_irq *desc = vgic_irq_desc(vcpu, virq);

		if (desc != NULL) {
			desc->pending = ((state & ICH_LR_STATE_PENDING) != 0U);
			desc->active = ((state & ICH_LR_STATE_ACTIVE) != 0U);
		}

		if ((state == ICH_LR_STATE_INVALID) || (state == 0U)) {
			remove_lr(vcpu, idx);
		} else {
			idx++;
		}
	}
}

void arm64_vgicv3_sync_vcpu(struct acrn_vcpu *vcpu)
{
	vgicv3_sync_vcpu(vcpu, false);
}

void arm64_vgicv3_sync_current_vcpu(struct acrn_vcpu *vcpu)
{
	vgicv3_sync_vcpu(vcpu, true);
}

static void vgicv3_flush_vcpu(struct acrn_vcpu *vcpu, bool is_current)
{
	struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
	struct arm64_vgicv3_vcpu_ctx *ctx = &vcpu->arch.vgic;
	uint32_t virq;

	/*
	 * Flush chooses pending, enabled virtual IRQs and materializes them into
	 * hardware LRs. If no LR is available, underflow/maintenance signaling is
	 * enabled so the guest exit path can retry after the guest consumes entries.
	 */
	ctx->hcr &= ~ICH_HCR_UIE;

	for (virq = 0U; virq < ARM64_VGIC_IRQ_NUM; virq++) {
		struct arm64_vgic_irq *desc = &vgic->irq[vcpu->vcpu_id][virq];
		int32_t lr_idx;

		if (!desc->enabled || !desc->pending) {
			continue;
		}

		lr_idx = find_lr_for_virq(vcpu, virq);
		if (lr_idx >= 0) {
			ctx->lr[lr_idx] = make_lr(desc);
			continue;
		}

		if (ctx->used_lrs >= vgic->lr_count) {
			ctx->hcr |= ICH_HCR_UIE;
			break;
		}

		ctx->lr[ctx->used_lrs] = make_lr(desc);
		ctx->used_lrs++;
		desc->pending = false;
		desc->active = desc->level;
	}

	vgicv3_write_loaded_lrs(vcpu, is_current);
}

void arm64_vgicv3_flush_vcpu(struct acrn_vcpu *vcpu)
{
	vgicv3_flush_vcpu(vcpu, false);
}

void arm64_vgicv3_flush_current_vcpu(struct acrn_vcpu *vcpu)
{
	vgicv3_flush_vcpu(vcpu, true);
}

int32_t arm64_vgicv3_inject_irq(struct acrn_vcpu *vcpu, uint32_t virq, bool level)
{
	struct arm64_vgic_irq *desc = vgic_irq_desc(vcpu, virq);
	struct arm64_vgicv3 *vgic;
	uint64_t flags;
	int32_t ret = -EINVAL;

	if (desc != NULL) {
		vgic = &vcpu->vm->arch_vm.vgic;
		spinlock_irqsave_obtain(&vgic->lock, &flags);
		/*
		 * Always sync before injection so a guest-completed LR does not
		 * get overwritten by stale software state. The event request wakes
		 * a blocked vCPU and makes the scheduler re-enter pre-guest
		 * request processing.
		 */
		arm64_vgicv3_sync_vcpu(vcpu);
		desc->level = level;
		if (!desc->enabled) {
			desc->enabled = true;
		}
		desc->pending = true;
		if (vgicv3_vcpu_is_loaded(vcpu)) {
			arm64_vgicv3_flush_vcpu(vcpu);
		}
		spinlock_irqrestore_release(&vgic->lock, flags);
		vcpu_make_request(vcpu, ARM64_VCPU_REQUEST_EVENT);
		signal_event(&vcpu->events[ARM64_VCPU_EVENT_VIRTUAL_INTERRUPT]);
		ret = 0;
	}

	return ret;
}

int32_t arm64_vgicv3_clear_irq(struct acrn_vcpu *vcpu, uint32_t virq)
{
	struct arm64_vgic_irq *desc = vgic_irq_desc(vcpu, virq);
	struct arm64_vgicv3 *vgic;
	uint64_t flags;
	int32_t ret = -EINVAL;

	if (desc != NULL) {
		int32_t lr_idx;

		vgic = &vcpu->vm->arch_vm.vgic;
		spinlock_irqsave_obtain(&vgic->lock, &flags);
		arm64_vgicv3_sync_vcpu(vcpu);
		lr_idx = find_lr_for_virq(vcpu, virq);
		if (lr_idx >= 0) {
			remove_lr(vcpu, (uint32_t)lr_idx);
		}
		desc->pending = false;
		desc->active = false;
		desc->level = false;
		if (vgicv3_vcpu_is_loaded(vcpu)) {
			arm64_vgicv3_flush_vcpu(vcpu);
		}
		spinlock_irqrestore_release(&vgic->lock, flags);
		vcpu_make_request(vcpu, ARM64_VCPU_REQUEST_EVENT);
		ret = 0;
	}

	return ret;
}

static bool sgi1r_targets_vcpu(uint64_t value, uint16_t source_vcpu_id,
	uint16_t target_vcpu_id)
{
	bool targeted;

	if ((value & ICC_SGI1R_IRM) != 0UL) {
		targeted = (target_vcpu_id != source_vcpu_id);
	} else {
		uint64_t aff1 = (value >> ICC_SGI1R_AFF1_SHIFT) & ICC_SGI1R_AFF_MASK;
		uint64_t aff2 = (value >> ICC_SGI1R_AFF2_SHIFT) & ICC_SGI1R_AFF_MASK;
		uint64_t aff3 = (value >> ICC_SGI1R_AFF3_SHIFT) & ICC_SGI1R_AFF_MASK;
		uint64_t target_list = value & ICC_SGI1R_TARGET_LIST_MASK;

		targeted = (aff1 == 0UL) && (aff2 == 0UL) && (aff3 == 0UL) &&
			((target_list & (1UL << target_vcpu_id)) != 0UL);
	}

	return targeted;
}

int32_t arm64_vgicv3_handle_sgi1r(struct acrn_vcpu *vcpu, uint64_t value)
{
	uint32_t intid = (uint32_t)((value >> ICC_SGI1R_INTID_SHIFT) & ICC_SGI1R_INTID_MASK);
	uint16_t idx;
	struct acrn_vcpu *target_vcpu;

	/*
	 * Guest SGI sends are trapped from ICC_SGI1R_EL1 and translated into
	 * per-target virtual IRQ injections. The current affinity model matches
	 * the QEMU vMPIDR layout where vCPU IDs occupy affinity level 0.
	 */
	foreach_vcpu(idx, vcpu->vm, target_vcpu) {
		if (sgi1r_targets_vcpu(value, vcpu->vcpu_id, target_vcpu->vcpu_id)) {
			(void)arm64_vgicv3_inject_irq(target_vcpu, intid, false);
			if (target_vcpu->state == VCPU_RUNNING) {
				kick_vcpu(target_vcpu);
			}
		}
	}

	return 0;
}

void arm64_vgicv3_maintenance_irq_handler(__unused uint32_t irq, __unused void *data)
{
	struct acrn_vcpu *vcpu = get_running_vcpu(get_pcpu_id());

	if (vcpu != NULL) {
		struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
		uint64_t flags;

		/*
		 * Maintenance IRQs are the hardware notification that LR state
		 * needs service. Read back guest-consumed state, then refill LRs
		 * from software pending state while still on the running pCPU.
		 */
		spinlock_irqsave_obtain(&vgic->lock, &flags);
		arm64_vgicv3_sync_current_vcpu(vcpu);
		arm64_vgicv3_flush_current_vcpu(vcpu);
		spinlock_irqrestore_release(&vgic->lock, flags);
	}
}

void arm64_vgicv3_virtual_timer_irq_handler(__unused uint32_t irq, __unused void *data)
{
	struct acrn_vcpu *vcpu = get_running_vcpu(get_pcpu_id());
	uint32_t ctl;

	/*
	 * CNTV is the hardware timer context loaded for the current vCPU. Its PPI
	 * is physically delivered to the pCPU running that vCPU, then translated
	 * into the guest-visible PPI recorded in timer_virq. That lets CNTP-based
	 * RTOS guests keep their expected interrupt number while the host still
	 * reserves CNTP for scheduler ticks.
	 */
	if (vcpu != NULL) {
		uint32_t virq = vcpu->arch.gctx.timer_virq;

		/*
		 * The hardware virtual timer is level-triggered. Once the deadline
		 * expires, the PPI remains asserted until EL1 programs a new deadline
		 * or masks/disables the timer. Mask it while injecting the virtual IRQ
		 * so EL2 does not loop on the same expired deadline before the guest
		 * timer handler has a chance to run.
		 */
		ctl = read_cntv_ctl_el0();
		write_cntv_ctl_el0((ctl & (CNTV_CTL_ENABLE | CNTV_CTL_IMASK)) | CNTV_CTL_IMASK);
		if (virq == 0U) {
			virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
		}
		(void)arm64_vgicv3_inject_irq(vcpu, virq, true);
	}
}

static uint32_t reg_word_index(uint32_t offset, uint32_t base)
{
	return (offset - base) / 4U;
}

static bool mmio_access_in_range(uint32_t offset, uint64_t size, uint32_t start, uint32_t end)
{
	return (offset >= start) && (offset < end) && (size <= (uint64_t)(end - offset));
}

static bool vgic_priority_access(uint32_t offset, uint64_t size)
{
	/*
	 * Priority registers are byte-addressable by architecture, and guests may
	 * legally use byte, halfword, or word accesses. Other modeled registers
	 * are handled as aligned 32-bit words by vgic_word_access().
	 */
	return ((size == 1UL) || (size == 2UL) || (size == 4UL)) &&
		mmio_access_in_range(offset, size, VGICD_IPRIORITYR, VGICD_ICFGR);
}

static bool vgic_word_access(uint32_t offset, uint64_t size)
{
	return (size == 4UL) && ((offset & 0x3U) == 0U);
}

struct vgic_irq_bit_reg {
	uint32_t selector;
	uint32_t irq_base;
};

static bool vgic_decode_irq_bit_reg(uint32_t offset, bool local_bank,
	struct vgic_irq_bit_reg *reg)
{
	uint32_t selector = UINT32_MAX;
	uint32_t base = 0U;
	bool ret = true;

	/*
	 * The GIC enable/pending/active registers are arranged as parallel
	 * set/clear views over the same IRQ descriptor bits. Decode the offset
	 * once into the canonical selector and first INTID covered by the word.
	 *
	 * Redistributor SGI/PPI accesses use local_bank=true because the current
	 * model exposes only the first 32 local INTIDs for each target vCPU.
	 */
	switch (offset) {
	case VGICD_ISENABLER ... (VGICD_ICENABLER - 1U):
		selector = VGICD_ISENABLER;
		base = reg_word_index(offset, VGICD_ISENABLER) * 32U;
		break;
	case VGICD_ICENABLER ... (VGICD_ISPENDR - 1U):
		selector = VGICD_ICENABLER;
		base = reg_word_index(offset, VGICD_ICENABLER) * 32U;
		break;
	case VGICD_ISPENDR ... (VGICD_ICPENDR - 1U):
		selector = VGICD_ISPENDR;
		base = reg_word_index(offset, VGICD_ISPENDR) * 32U;
		break;
	case VGICD_ICPENDR ... (VGICD_ISACTIVER - 1U):
		selector = VGICD_ICPENDR;
		base = reg_word_index(offset, VGICD_ICPENDR) * 32U;
		break;
	case VGICD_ISACTIVER ... (VGICD_ICACTIVER - 1U):
		selector = VGICD_ISACTIVER;
		base = reg_word_index(offset, VGICD_ISACTIVER) * 32U;
		break;
	case VGICD_ICACTIVER ... (VGICD_IPRIORITYR - 1U):
		selector = VGICD_ICACTIVER;
		base = reg_word_index(offset, VGICD_ICACTIVER) * 32U;
		break;
	default:
		ret = false;
		break;
	}

	if (ret) {
		reg->selector = selector;
		reg->irq_base = local_bank ? 0U : base;
	}

	return ret;
}

static uint64_t vgic_read_irq_priority(struct acrn_vcpu *vcpu, uint32_t irq_base, uint64_t size)
{
	uint64_t value = 0UL;
	uint32_t byte;

	for (byte = 0U; byte < size; byte++) {
		struct arm64_vgic_irq *desc = vgic_irq_desc(vcpu, irq_base + byte);
		uint8_t priority = (desc != NULL) ? desc->priority : GIC_PRIORITY_DEFAULT;

		value |= (uint64_t)priority << (byte * 8U);
	}

	return value;
}

static void vgic_write_irq_priority(struct acrn_vcpu *vcpu, uint32_t irq_base,
	uint64_t size, uint64_t value)
{
	uint32_t byte;

	for (byte = 0U; byte < size; byte++) {
		struct arm64_vgic_irq *desc = vgic_irq_desc(vcpu, irq_base + byte);

		if (desc != NULL) {
			desc->priority = (uint8_t)((value >> (byte * 8U)) & 0xffU);
		}
	}
}

static uint32_t vgic_read_irq_bits(struct acrn_vcpu *vcpu, uint32_t irq_base, uint32_t selector)
{
	uint32_t val = 0U;
	uint32_t bit;

	for (bit = 0U; bit < 32U; bit++) {
		struct arm64_vgic_irq *desc = vgic_irq_desc(vcpu, irq_base + bit);
		bool set = false;

		if (desc == NULL) {
			continue;
		}

		switch (selector) {
		case VGICD_ISENABLER:
		case VGICD_ICENABLER:
			set = desc->enabled;
			break;
		case VGICD_ISPENDR:
		case VGICD_ICPENDR:
			set = desc->pending;
			break;
		case VGICD_ISACTIVER:
		case VGICD_ICACTIVER:
			set = desc->active;
			break;
		default:
			break;
		}

		if (set) {
			val |= BIT32(bit);
		}
	}

	return val;
}

static void vgic_write_irq_bits(struct acrn_vcpu *vcpu, uint32_t irq_base, uint32_t selector, uint32_t val)
{
	uint32_t bit;

	for (bit = 0U; bit < 32U; bit++) {
		struct arm64_vgic_irq *desc;

		if ((val & BIT32(bit)) == 0U) {
			continue;
		}

		desc = vgic_irq_desc(vcpu, irq_base + bit);
		if (desc == NULL) {
			continue;
		}

		switch (selector) {
		case VGICD_ISENABLER:
			desc->enabled = true;
			break;
		case VGICD_ICENABLER:
			desc->enabled = false;
			break;
		case VGICD_ISPENDR:
			desc->pending = true;
			break;
		case VGICD_ICPENDR:
			desc->pending = false;
			break;
		case VGICD_ISACTIVER:
			desc->active = true;
			break;
		case VGICD_ICACTIVER:
			desc->active = false;
			break;
		default:
			break;
		}
	}
}

static void vgicd_mmio_read(struct acrn_vcpu *vcpu, struct arm64_vgicv3 *vgic,
	struct acrn_mmio_request *mmio, uint32_t offset)
{
	struct vgic_irq_bit_reg bit_reg;

	/*
	 * Distributor MMIO exposes the shared interrupt-controller programming
	 * model. For the current QEMU guests, the important pieces are enable,
	 * pending, active, priority, and basic identity registers.
	 */
	if (vgic_decode_irq_bit_reg(offset, false, &bit_reg)) {
		mmio->value = vgic_read_irq_bits(vcpu, bit_reg.irq_base, bit_reg.selector);
	} else {
		switch (offset) {
		case VGICD_CTLR:
			mmio->value = vgic->gicd_ctlr;
			break;
		case VGICD_TYPER:
			mmio->value = vgic->gicd_typer;
			break;
		case VGICD_IIDR:
			mmio->value = arm64_platform_gic_iidr();
			break;
		case VGICD_IROUTER ... (VGICD_PIDR2 - 1U):
			mmio->value = 0UL;
			break;
		case VGICD_PIDR2:
			mmio->value = vgic->gicd_pidr2;
			break;
		default:
			mmio->value = 0UL;
			break;
		}
	}
}

static bool vgicd_mmio_write(struct acrn_vcpu *vcpu, struct arm64_vgicv3 *vgic,
	const struct acrn_mmio_request *mmio, uint32_t offset)
{
	struct vgic_irq_bit_reg bit_reg;
	uint32_t value = (uint32_t)mmio->value;
	bool flush = false;

	/*
	 * Only writes that can change pending/active/enable/priority state need
	 * to be flushed into LRs. Control or ignored register writes update the
	 * software model only and avoid unnecessary hardware LR traffic.
	 */
	if (vgic_decode_irq_bit_reg(offset, false, &bit_reg)) {
		vgic_write_irq_bits(vcpu, bit_reg.irq_base, bit_reg.selector, value);
		flush = true;
	} else {
		switch (offset) {
		case VGICD_CTLR:
			vgic->gicd_ctlr = value;
			break;
		default:
			break;
		}
	}

	return flush;
}

static int32_t vgicd_mmio_access(struct acrn_vcpu *vcpu, struct arm64_vgicv3 *vgic,
	struct acrn_mmio_request *mmio)
{
	uint32_t offset = (uint32_t)(mmio->address - arm64_platform_guest_gicd_base(vcpu->vm->vm_id));
	uint32_t irq_base;
	int32_t ret = 0;

	/*
	 * Dispatch order mirrors architectural access rules: priority registers
	 * accept sub-word accesses, while the rest of the modeled distributor
	 * block is restricted to aligned 32-bit accesses.
	 */
	if (vgic_priority_access(offset, mmio->size)) {
		irq_base = offset - VGICD_IPRIORITYR;
		if (mmio->direction == ACRN_IOREQ_DIR_READ) {
			mmio->value = vgic_read_irq_priority(vcpu, irq_base, mmio->size);
		} else {
			vgic_write_irq_priority(vcpu, irq_base, mmio->size, mmio->value);
			arm64_vgicv3_flush_vcpu(vcpu);
		}
	} else if (!vgic_word_access(offset, mmio->size)) {
		ret = -EINVAL;
	} else if (mmio->direction == ACRN_IOREQ_DIR_READ) {
		vgicd_mmio_read(vcpu, vgic, mmio, offset);
	} else {
		if (vgicd_mmio_write(vcpu, vgic, mmio, offset)) {
			arm64_vgicv3_flush_vcpu(vcpu);
		}
	}

	return ret;
}

static uint16_t vgicr_vcpu_id(const struct acrn_vcpu *vcpu, uint64_t addr)
{
	return (uint16_t)((addr - arm64_platform_guest_gicr_base(vcpu->vm->vm_id)) /
		arm64_platform_guest_gicr_stride(vcpu->vm->vm_id));
}

static uint32_t vgicr_offset(const struct acrn_vcpu *vcpu, uint64_t addr)
{
	return (uint32_t)((addr - arm64_platform_guest_gicr_base(vcpu->vm->vm_id)) %
		arm64_platform_guest_gicr_stride(vcpu->vm->vm_id));
}

static uint64_t vgicr_typer_value(const struct acrn_vm *vm, uint16_t target_vcpu_id)
{
	uint64_t typer = ((uint64_t)target_vcpu_id << VGICR_TYPER_PROCNUM_SHIFT) |
		((uint64_t)target_vcpu_id << VGICR_TYPER_AFF_SHIFT);

	if (target_vcpu_id == (vm->hw.created_vcpus - 1U)) {
		typer |= VGICR_TYPER_LAST;
	}

	return typer;
}

static void vgicr_write_waker(struct arm64_vgicv3 *vgic, uint16_t target_vcpu_id, uint32_t value)
{
	uint32_t waker = 0U;

	if ((value & VGICR_WAKER_PROCESSOR_SLEEP) != 0U) {
		waker = VGICR_WAKER_PROCESSOR_SLEEP | VGICR_WAKER_CHILDREN_ASLEEP;
	}

	vgic->gicr_waker[target_vcpu_id] = waker;
}

static void vgicr_sgi_mmio_read(struct acrn_vcpu *target_vcpu,
	struct acrn_mmio_request *mmio, uint32_t sgi_offset)
{
	struct vgic_irq_bit_reg bit_reg;
	uint32_t irq_base;

	/*
	 * The SGI/PPI sub-frame reuses distributor-style register offsets, but the
	 * current model exposes only the target vCPU's local INTID bank.
	 */
	if (vgic_decode_irq_bit_reg(sgi_offset, true, &bit_reg)) {
		mmio->value = vgic_read_irq_bits(target_vcpu, bit_reg.irq_base, bit_reg.selector);
	} else {
		switch (sgi_offset) {
		case VGICD_IPRIORITYR ... (VGICD_ICFGR - 1U):
			irq_base = sgi_offset - VGICD_IPRIORITYR;
			mmio->value = vgic_read_irq_priority(target_vcpu, irq_base, mmio->size);
			break;
		default:
			mmio->value = 0UL;
			break;
		}
	}
}

static bool vgicr_sgi_mmio_write(struct acrn_vcpu *target_vcpu,
	const struct acrn_mmio_request *mmio, uint32_t sgi_offset)
{
	struct vgic_irq_bit_reg bit_reg;
	uint32_t irq_base;
	uint32_t value = (uint32_t)mmio->value;
	bool flush = false;

	if (vgic_decode_irq_bit_reg(sgi_offset, true, &bit_reg)) {
		vgic_write_irq_bits(target_vcpu, bit_reg.irq_base, bit_reg.selector, value);
		flush = true;
	} else {
		switch (sgi_offset) {
		case VGICD_IPRIORITYR ... (VGICD_ICFGR - 1U):
			irq_base = sgi_offset - VGICD_IPRIORITYR;
			vgic_write_irq_priority(target_vcpu, irq_base, mmio->size, mmio->value);
			flush = true;
			break;
		default:
			break;
		}
	}

	return flush;
}

static void vgicr_mmio_read(struct acrn_vcpu *vcpu, struct arm64_vgicv3 *vgic,
	struct acrn_vcpu *target_vcpu, struct acrn_mmio_request *mmio,
	uint16_t target_vcpu_id, uint32_t offset)
{
	uint64_t typer;

	/*
	 * Redistributor frames are per-vCPU. The top frame exposes identity and
	 * sleep state, while the SGI/PPI sub-frame reuses the distributor-style
	 * helpers against the target vCPU row.
	 */
	switch (offset) {
	case VGICR_CTLR:
		mmio->value = 0UL;
		break;
	case VGICR_IIDR:
		mmio->value = arm64_platform_gic_iidr();
		break;
	case VGICR_TYPER:
		typer = vgicr_typer_value(vcpu->vm, target_vcpu_id);
		mmio->value = typer & UINT32_MAX;
		break;
	case VGICR_TYPER + 4U:
		typer = vgicr_typer_value(vcpu->vm, target_vcpu_id);
		mmio->value = typer >> 32U;
		break;
	case VGICR_WAKER:
		mmio->value = vgic->gicr_waker[target_vcpu_id];
		break;
	case VGICR_PIDR2:
		mmio->value = GIC_PIDR2_ARCH_GICV3;
		break;
	case VGICR_SGI_BASE ... (VGICR_SGI_END - 1U):
		vgicr_sgi_mmio_read(target_vcpu, mmio, offset - VGICR_SGI_BASE);
		break;
	default:
		mmio->value = 0UL;
		break;
	}
}

static bool vgicr_mmio_write(struct arm64_vgicv3 *vgic, struct acrn_vcpu *target_vcpu,
	const struct acrn_mmio_request *mmio, uint16_t target_vcpu_id, uint32_t offset)
{
	bool flush = false;

	switch (offset) {
	case VGICR_WAKER:
		vgicr_write_waker(vgic, target_vcpu_id, (uint32_t)mmio->value);
		break;
	case VGICR_SGI_BASE ... (VGICR_SGI_END - 1U):
		flush = vgicr_sgi_mmio_write(target_vcpu, mmio, offset - VGICR_SGI_BASE);
		break;
	default:
		break;
	}

	return flush;
}

static int32_t vgicr_mmio_access(struct acrn_vcpu *vcpu, struct arm64_vgicv3 *vgic,
	struct acrn_mmio_request *mmio)
{
	uint16_t target_vcpu_id = vgicr_vcpu_id(vcpu, mmio->address);
	uint32_t offset = vgicr_offset(vcpu, mmio->address);
	struct acrn_vcpu *target_vcpu;
	uint32_t sgi_offset;
	uint32_t irq_base;
	int32_t ret = 0;

	/*
	 * The redistributor has two access domains: a top-level per-vCPU frame and
	 * an SGI/PPI sub-frame. The sub-frame inherits distributor-style priority
	 * byte access; other modeled redistributor registers are 32-bit words.
	 */
	if (target_vcpu_id >= vcpu->vm->hw.created_vcpus) {
		ret = -EINVAL;
	} else {
		target_vcpu = vcpu_from_vid(vcpu->vm, target_vcpu_id);
		if ((offset >= VGICR_SGI_BASE) &&
			vgic_priority_access(offset - VGICR_SGI_BASE, mmio->size)) {
			sgi_offset = offset - VGICR_SGI_BASE;
			irq_base = sgi_offset - VGICD_IPRIORITYR;
			if (mmio->direction == ACRN_IOREQ_DIR_READ) {
				mmio->value = vgic_read_irq_priority(target_vcpu, irq_base, mmio->size);
			} else {
				vgic_write_irq_priority(target_vcpu, irq_base, mmio->size, mmio->value);
				arm64_vgicv3_flush_vcpu(target_vcpu);
			}
		} else if (!vgic_word_access(offset, mmio->size)) {
			ret = -EINVAL;
		} else if (mmio->direction == ACRN_IOREQ_DIR_READ) {
			vgicr_mmio_read(vcpu, vgic, target_vcpu, mmio, target_vcpu_id, offset);
		} else {
			if (vgicr_mmio_write(vgic, target_vcpu, mmio, target_vcpu_id, offset)) {
				arm64_vgicv3_flush_vcpu(target_vcpu);
			}
		}
	}

	return ret;
}

static int32_t vgicv3_mmio_dispatch(struct acrn_vcpu *vcpu, struct arm64_vgicv3 *vgic,
	struct acrn_mmio_request *mmio)
{
	int32_t ret = -ENODEV;

	if (addr_in_range(mmio->address, arm64_platform_guest_gicd_base(vcpu->vm->vm_id),
		arm64_platform_guest_gicd_size(vcpu->vm->vm_id))) {
		ret = vgicd_mmio_access(vcpu, vgic, mmio);
	} else if (addr_in_range(mmio->address, arm64_platform_guest_gicr_base(vcpu->vm->vm_id),
		arm64_platform_guest_gicr_size(vcpu->vm->vm_id))) {
		ret = vgicr_mmio_access(vcpu, vgic, mmio);
	}

	return ret;
}

static int32_t vgicv3_mmio_dispatch64(struct acrn_vcpu *vcpu, struct arm64_vgicv3 *vgic,
	struct acrn_mmio_request *mmio)
{
	struct acrn_mmio_request low = *mmio;
	struct acrn_mmio_request high = *mmio;
	int32_t ret;

	/*
	 * Some guest code issues 64-bit accesses to GIC registers even when the
	 * modeled register block is naturally 32-bit. Split them into two ordered
	 * word accesses so existing register handlers remain simple.
	 */
	low.size = 4UL;
	high.size = 4UL;
	high.address = mmio->address + 4UL;
	if (mmio->direction == ACRN_IOREQ_DIR_WRITE) {
		low.value = mmio->value & 0xffffffffUL;
		high.value = mmio->value >> 32U;
	}

	ret = vgicv3_mmio_dispatch(vcpu, vgic, &low);
	if (ret == 0) {
		ret = vgicv3_mmio_dispatch(vcpu, vgic, &high);
	}
	if ((ret == 0) && (mmio->direction == ACRN_IOREQ_DIR_READ)) {
		mmio->value = (low.value & 0xffffffffUL) | (high.value << 32U);
	}

	return ret;
}

int32_t arm64_vgicv3_mmio_handler(struct io_request *io_req, void *handler_private_data)
{
	struct arm64_vgicv3 *vgic = (struct arm64_vgicv3 *)handler_private_data;
	struct acrn_mmio_request *mmio = &io_req->reqs.mmio_request;
	struct acrn_vcpu *vcpu = get_running_vcpu(get_pcpu_id());
	uint64_t flags;
	int32_t ret = -ENODEV;

	/*
	 * MMIO can race with host-side virtual IRQ injection. The vGIC lock makes
	 * guest register programming, LR readback, and software IRQ descriptors a
	 * single coherent state transition.
	 */
	if ((vcpu != NULL) && (vgic != NULL)) {
		spinlock_irqsave_obtain(&vgic->lock, &flags);
		arm64_vgicv3_sync_current_vcpu(vcpu);
		if (mmio->size == 8UL) {
			ret = vgicv3_mmio_dispatch64(vcpu, vgic, mmio);
		} else {
			ret = vgicv3_mmio_dispatch(vcpu, vgic, mmio);
		}
		spinlock_irqrestore_release(&vgic->lock, flags);
	}

	return ret;
}
