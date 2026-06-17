/*
 * Copyright (C) 2026 Hustler Lo.
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
#include <guest_memory.h>
#include <ticks.h>
#include <timer.h>
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
 *
 * Interrupt virtualization flow:
 *
 *   guest ICC/GIC access or host event
 *        -> vGIC descriptor + pending bitmap
 *        -> vgicv3_sync_vcpu() reads completed LR state
 *        -> vgicv3_flush_vcpu() writes deliverable IRQs into LRs
 *        -> guest IAR/EOIR consumes the virtual interrupt
 *        -> maintenance IRQ or next sync reconciles active/pending state
 *
 * vtimer virtualization flow:
 *
 *   guest CNTP/CNTV sysreg write or live CNTV PPI
 *        -> vCPU timer shadow + live CNTV sample
 *        -> virtual timer IRQ descriptor and pending bitmap
 *        -> LR delivery to EL1
 *        -> guest reprogram/EOI lowers or completes the level source
 *
 * Linux SMP boot depends on a precise edge-SGI lifecycle. A representative
 * failure was VM2 reaching the PL011 console and then stalling with CPU0 in
 * smp_call_function_many_cond(), waiting for CPU2 to unlock a synchronous CSD
 * queued by kick_all_cpus_sync(). dumpstat showed that vCPU requests had been
 * consumed, SGI1 had been targeted and flushed into a list register, and the
 * SGI was no longer pending/active in the saved vGIC state; however the Linux
 * CSD flags stayed locked. Symbolication of the CSD function identified the
 * work item as do_nothing(), so the missing event was not a timer callback but
 * the normal IPI_CALL_FUNC handler running on the target CPU.
 *
 * The target CPUs were stopped around cpu_do_idle(): WFI had returned, but the
 * saved guest PSTATE still had IRQs masked because Linux enters WFI with local
 * interrupts disabled and later calls local_irq_enable() in the idle loop. The
 * virtual SGI may therefore wake the CPU before EL1 is ready to acknowledge and
 * dispatch a normal IRQ. If EL2 observes a pending-only edge LR disappear and
 * clears the software pending bit without EOI evidence, Linux loses the SGI
 * before local_irq_enable() can take it; the CSD remains locked and the boot CPU
 * spins forever. For this reason software-backed edge IRQs, especially SGIs,
 * request EOI maintenance even from pending-only state and are kept pending
 * until an explicit LR EOI/deactivation path proves that the guest completed
 * the interrupt.
 *
 * The Linux virtual timer has a related post-boot diagnostic signature: EL2 can
 * show an expired, host-masked CNTV source and a pending-only timer LR while
 * Linux reports that RCU timer wakeups did not happen. Do not solve this by
 * marking pending-only timer LRs for EOI maintenance: Linux can EOI such an LR
 * before the timer line has been lowered, causing EL2 to resample the still-high
 * CNTV source and immediately rebuild the same LR in a maintenance storm. Timer
 * repair must instead keep the CNTV line model and host mask in sync when EL2
 * already owns an in-flight timer LR.
 */
#define BIT32(n)			(1U << (n))

#define VGIC_VTIMER_STUCK_RESCUE_US	500U

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
#define VGICD_ITARGETSR		0x0800U
#define VGICD_ICFGR			0x0C00U
#define VGICD_IGRPMODR		0x0D00U
#define VGICD_IROUTER			0x6000U
#define VGICD_PIDR2			0xFFE8U

#define VGICR_CTLR			0x0000U
#define VGICR_IIDR			0x0004U
#define VGICR_TYPER			0x0008U
#define VGICR_WAKER			0x0014U
#define VGICR_PROPBASER		0x0070U
#define VGICR_PENDBASER		0x0078U
#define VGICR_INVLPIR			0x00A0U
#define VGICR_INVALLR			0x00B0U
#define VGICR_SYNCR			0x00C0U
#define VGICR_PIDR2			0xFFE8U
#define VGICR_SGI_BASE			0x10000U
#define VGICR_SGI_END			(VGICR_SGI_BASE + 0x10000U)

#define VGICR_CTLR_ENABLE_LPIS	BIT32(0U)
#define VGICR_CTLR_RWP		BIT32(3U)
#define VGIC_CTLR_ENABLE_G1		BIT32(1U)
#define VGIC_CTLR_ARE_NS		BIT32(5U)
#define VGICD_CTLR_RWP		BIT32(31U)
#define VGICD_TYPER_LPIS		BIT32(17U)
#define VGICD_TYPER_NUM_LPIS_SHIFT	11U
#define VGICD_TYPER_IDBITS_SHIFT	19U
#define VGICR_TYPER_LAST		(1UL << 4U)
#define VGICR_TYPER_PLPIS		(1UL << 0U)
#define VGICR_TYPER_PROCNUM_SHIFT	8U
#define VGICR_TYPER_AFF_SHIFT		32U
#define VGICR_WAKER_PROCESSOR_SLEEP	BIT32(1U)
#define VGICR_WAKER_CHILDREN_ASLEEP	BIT32(2U)
#define VGIC_VMCR_GROUP_ENABLES		ICH_VMCR_VENG1
#define VGICR_WAKER_READ_MASK		(VGICR_WAKER_PROCESSOR_SLEEP | VGICR_WAKER_CHILDREN_ASLEEP)

#define ICC_SGI1R_TARGET_LIST_MASK	0xffffUL
#define ICC_SGI1R_AFF1_SHIFT		16U
#define ICC_SGI1R_INTID_SHIFT		24U
#define ICC_SGI1R_INTID_MASK		0xfUL
#define ICC_SGI1R_IRM			(1UL << 40U)
#define ICC_SGI1R_AFF2_SHIFT		32U
#define ICC_SGI1R_AFF3_SHIFT		48U
#define ICC_SGI1R_AFF_MASK		0xffUL

#define GIC_IROUTER_IRM		(1UL << 31U)
#define GIC_IROUTER_AFF0_MASK		0xffUL
#define GIC_IROUTER_AFF1_SHIFT		8U
#define GIC_IROUTER_AFF2_SHIFT		16U
#define GIC_IROUTER_AFF3_SHIFT		32U
#define GIC_IROUTER_AFF_MASK		0xffUL
#define GIC_IROUTER_LOW_MASK		0xffffffffUL
#define GIC_PRIORITY_DEFAULT		0x80U
#define GIC_PRIORITY_LOWEST		0xffU
#define GIC_PIDR2_ARCH_GICV3		(3U << 4U)
#define VGIC_ICFGR_IRQS_PER_REG	16U
#define VGIC_INVALID_VCPU_ID		0xffffU
#define VGIC_INVALID_AFFINITY		0xffUL

#define GITS_CTLR			0x0000U
#define GITS_IIDR			0x0004U
#define GITS_TYPER			0x0008U
#define GITS_MPIDR			0x0018U
#define GITS_CBASER			0x0080U
#define GITS_CWRITER			0x0088U
#define GITS_CREADR			0x0090U
#define GITS_BASER			0x0100U
#define GITS_TRANSLATER		0x10040U
#define GITS_PIDR2			0xFFE8U
#define GITS_CIDR0			0xFFF0U
#define GITS_CIDR1			0xFFF4U
#define GITS_CIDR2			0xFFF8U
#define GITS_CIDR3			0xFFFCU

#define GITS_CTLR_ENABLE		BIT32(0U)
#define GITS_CTLR_QUIESCENT		BIT32(31U)
#define GITS_CBASER_VALID		(1UL << 63U)
#define GITS_CBASER_ADDRESS_MASK	0x0000fffffffff000UL
#define GITS_CBASER_SIZE_MASK		0xffUL
#define GITS_BASER_VALID		(1UL << 63U)
#define GITS_CMD_ITT_ADDRESS_MASK	0x000fffffffffff00UL
#define GITS_BASER_TYPE_SHIFT		56U
#define GITS_BASER_ENTRY_SIZE_SHIFT	48U
#define GITS_BASER_TYPE_DEVICE	1U
#define GITS_BASER_TYPE_COLLECTION	4U
#define GITS_TYPER_PLPIS		(1UL << 0U)
#define GITS_TYPER_ITT_ENTRY_SIZE_SHIFT	4U
#define GITS_TYPER_IDBITS_SHIFT	8U
#define GITS_TYPER_DEVBITS_SHIFT	13U
#define GITS_CMD_SIZE			32U
#define GITS_CMD_MAPD			0x08U
#define GITS_CMD_MAPC			0x09U
#define GITS_CMD_MAPTI			0x0aU
#define GITS_CMD_MAPI			0x0bU
#define GITS_CMD_MOVI			0x01U
#define GITS_CMD_INT			0x03U
#define GITS_CMD_CLEAR			0x04U
#define GITS_CMD_SYNC			0x05U
#define GITS_CMD_INV			0x0cU
#define GITS_CMD_INVALL		0x0dU
#define GITS_CMD_MOVALL		0x0eU
#define GITS_CMD_DISCARD		0x0fU
#define GITS_DEVBITS			15U
#define GITS_ITT_ENTRY_SIZE		16U
#define GITS_CMD_QUEUE_BUDGET		1024U

static uint32_t vgic_lr_count;
static bool vgic_global_initialized;

static void vgicv3_sync_vcpu(struct acrn_vcpu *vcpu, bool is_current);
static void vgicv3_flush_vcpu(struct acrn_vcpu *vcpu, bool is_current);
static int32_t vgicv3_inject_current_timer(struct acrn_vcpu *vcpu, uint32_t virq,
	uint32_t guest_ctl, uint64_t guest_cval);
static bool vgicv3_vtimer_live_stuck(struct acrn_vcpu *vcpu);
static int32_t vgic_inject_locked(struct arm64_vgicv3 *vgic,
	struct acrn_vcpu *target_vcpu, uint32_t virq, bool level);
static struct arm64_vits_event *vits_find_event(struct arm64_vits *its,
	uint32_t device_id, uint32_t event_id);
static void vits_inject_event_locked(struct acrn_vm *vm, struct arm64_vgicv3 *vgic,
	struct arm64_vits_event *event);
static uint32_t vgic_lpi_index(uint32_t lpi);
static bool vgic_irq_is_lpi(uint32_t virq);

static uint32_t min_u32(uint32_t a, uint32_t b)
{
	return (a < b) ? a : b;
}

static bool addr_in_range(uint64_t addr, uint64_t base, uint64_t size)
{
	return (size != 0UL) && (addr >= base) && ((addr - base) < size);
}

static uint16_t arm64_vcpu_count_from_affinity(uint64_t affinity)
{
	uint16_t count = 0U;

	while (affinity != 0UL) {
		affinity &= affinity - 1UL;
		count++;
	}

	return count;
}

static struct arm64_vgic_irq *vgic_irq_desc(struct acrn_vcpu *vcpu, uint32_t virq)
{
	struct arm64_vgic_irq *desc = NULL;
	struct arm64_vgicv3 *vgic;

	if ((vcpu != NULL) && (vcpu->vm != NULL) && (vcpu->vcpu_id < ARM64_VGIC_MAX_VCPUS)) {
		vgic = &vcpu->vm->arch_vm.vgic;
		if (vgic->initialized) {
			if (virq < ARM64_VGIC_IRQ_NUM) {
				desc = &vgic->irq[vcpu->vcpu_id][virq];
			} else if (vgic->its_enabled && vgic_irq_is_lpi(virq)) {
				desc = &vgic->lpi[vcpu->vcpu_id][vgic_lpi_index(virq)];
			}
		}
	}

	return desc;
}

static void vgic_record_irq_injection(struct acrn_vcpu *source_vcpu,
	struct acrn_vcpu *target_vcpu, uint32_t virq, bool level, int32_t status)
{
	struct arm64_vcpu_last_irq *last;

	if (target_vcpu != NULL) {
		last = &target_vcpu->arch.debug.last_irq;
		last->virq = virq;
		last->status = status;
		last->source_vcpu_id = (source_vcpu != NULL) ?
			source_vcpu->vcpu_id : ARM64_VCPU_DEBUG_INVALID_VCPU_ID;
		last->target_vcpu_id = target_vcpu->vcpu_id;
		last->level = level;
		last->tsc = cpu_ticks();
	}
}

static uint32_t vgic_irq_word(uint32_t virq)
{
	return virq / 32U;
}

static uint32_t vgic_irq_bit(uint32_t virq)
{
	return virq % 32U;
}

static uint32_t vgic_lpi_index(uint32_t lpi)
{
	return lpi - ARM64_VGIC_LPI_BASE;
}

static bool vgic_irq_is_spi(uint32_t virq)
{
	return (virq >= ARM64_VGIC_LOCAL_IRQ_NUM) && (virq < ARM64_VGIC_IRQ_NUM);
}

static bool vgic_irq_is_lpi(uint32_t virq)
{
	return (virq >= ARM64_VGIC_LPI_BASE) &&
		(virq < (ARM64_VGIC_LPI_BASE + ARM64_VGIC_LPI_NUM));
}

static void vgic_set_pending(struct arm64_vgicv3 *vgic, uint16_t vcpu_id,
	struct arm64_vgic_irq *desc, bool pending)
{
	uint32_t virq = desc->virq;
	uint32_t word;
	uint32_t mask;

	desc->pending = pending;
	if (vgic_irq_is_lpi(virq)) {
		virq -= ARM64_VGIC_LPI_BASE;
		word = vgic_irq_word(virq);
		mask = BIT32(vgic_irq_bit(virq));
		if (pending) {
			vgic->lpi_pending_bitmap[vcpu_id][word] |= mask;
		} else {
			vgic->lpi_pending_bitmap[vcpu_id][word] &= ~mask;
		}
		return;
	}

	word = vgic_irq_word(virq);
	mask = BIT32(vgic_irq_bit(virq));
	if (pending) {
		vgic->pending_bitmap[vcpu_id][word] |= mask;
	} else {
		vgic->pending_bitmap[vcpu_id][word] &= ~mask;
	}
}

static uint32_t vgic_vtimer_guest_ctl(const struct arm64_vcpu_guest_ctx *gctx);

/*
 * Host masking hands the live CNTV output to the software vGIC while a timer
 * LR owns guest delivery:
 *
 *   CNTV expires -> mask host PPI -> inject virtual timer LR
 *   guest EOI/reprogram -> resample CNTV -> unmask when the line is safe
 */
static void vgic_timer_set_host_mask(struct acrn_vcpu *vcpu, bool masked)
{
	struct arm64_vcpu_guest_ctx *gctx;
	uint32_t ctl;

	if ((vcpu == NULL) || (get_running_vcpu(get_pcpu_id()) != vcpu)) {
		return;
	}

	gctx = &vcpu->arch.gctx;
	ctl = read_cntv_ctl_el0() & (CNTV_CTL_ENABLE | CNTV_CTL_IMASK);
	if (masked) {
		gctx->cntv_el2_masked = true;
		write_cntv_ctl_el0(ctl | CNTV_CTL_IMASK);
	} else if (gctx->cntv_el2_masked) {
		gctx->cntv_el2_masked = false;
		ctl = (ctl & ~CNTV_CTL_IMASK) | (vgic_vtimer_guest_ctl(gctx) & CNTV_CTL_IMASK);
		write_cntv_ctl_el0(ctl);
		arm64_gicv3_enable_irq(ARM64_GIC_PPI_VIRTUAL_TIMER);
	}
}

static uint32_t vgic_live_vtimer_ctl(struct acrn_vcpu *vcpu)
{
	struct arm64_vcpu_guest_ctx *gctx = &vcpu->arch.gctx;
	uint32_t ctl = read_cntv_ctl_el0() & (CNTV_CTL_ENABLE | CNTV_CTL_IMASK);

	if (gctx->cntv_el2_masked) {
		ctl = (ctl & ~CNTV_CTL_IMASK) | (vgic_vtimer_guest_ctl(gctx) & CNTV_CTL_IMASK);
	}

	return ctl;
}

static bool vgic_sample_current_vtimer(struct acrn_vcpu *vcpu)
{
	struct arm64_vcpu_guest_ctx *gctx = &vcpu->arch.gctx;
	uint32_t ctl = vgic_live_vtimer_ctl(vcpu);
	uint64_t cval = read_cntv_cval_el0();
	uint64_t now = read_cntvct_el0();

	/*
	 * CNTV is the hardware source for the guest timer while this vCPU is
	 * loaded. Sampling it at LR synchronization time prevents stale software
	 * pending state from re-injecting a timer interrupt after the guest has
	 * already programmed the next deadline.
	 */
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

static uint32_t vgic_vtimer_guest_ctl(const struct arm64_vcpu_guest_ctx *gctx)
{
	return (gctx->timer_virq == ARM64_GIC_PPI_PHYSICAL_TIMER) ?
		gctx->cntp_ctl_el0 : gctx->cntv_ctl_el0;
}

static uint64_t vgic_vtimer_guest_cval(const struct arm64_vcpu_guest_ctx *gctx)
{
	return (gctx->timer_virq == ARM64_GIC_PPI_PHYSICAL_TIMER) ?
		gctx->cntp_cval_el0 : gctx->cntv_cval_el0;
}

static uint64_t vgic_vtimer_host_deadline(const struct arm64_vcpu_guest_ctx *gctx)
{
	return vgic_vtimer_guest_cval(gctx) - gctx->cntvoff_el2;
}

static bool vgic_vtimer_guest_enabled(const struct arm64_vcpu_guest_ctx *gctx)
{
	uint32_t ctl = vgic_vtimer_guest_ctl(gctx);

	return ((ctl & CNTV_CTL_ENABLE) != 0U) && ((ctl & CNTV_CTL_IMASK) == 0U);
}

static bool vgic_vtimer_guest_expired(const struct arm64_vcpu_guest_ctx *gctx)
{
	return vgic_vtimer_guest_enabled(gctx) &&
		((int64_t)(vgic_vtimer_host_deadline(gctx) - cpu_ticks()) <= 0L);
}

static bool vgic_is_vtimer_irq(const struct acrn_vcpu *vcpu, uint32_t virq)
{
	uint32_t timer_virq = (vcpu->arch.gctx.timer_virq == 0U) ?
		ARM64_GIC_PPI_VIRTUAL_TIMER : vcpu->arch.gctx.timer_virq;

	return virq == timer_virq;
}

static uint64_t vgic_its_baser_type(uint32_t type, uint32_t entry_size)
{
	return ((uint64_t)type << GITS_BASER_TYPE_SHIFT) |
		((uint64_t)(entry_size - 1U) << GITS_BASER_ENTRY_SIZE_SHIFT);
}

static bool vits_offset_aligned(uint64_t offset)
{
	return (offset & (GITS_CMD_SIZE - 1UL)) == 0UL;
}

static void vgic_its_init(struct arm64_vgicv3 *vgic)
{
	struct arm64_vits *its = &vgic->its;
	uint32_t idx;

	(void)memset(its, 0U, sizeof(*its));
	/*
	 * Advertise a conservative ITS: physical LPIs only, 14-bit INTID space
	 * starting at 8192, 16-bit device IDs, and 16-byte ITT entries. vGICv4
	 * VLPIs are deliberately not exposed in this first model. The software
	 * vITS stores only a small active event window; it does not preallocate
	 * descriptors for the whole advertised LPI space.
	 */
	its->ctlr = GITS_CTLR_QUIESCENT;
	its->typer = GITS_TYPER_PLPIS |
		((uint64_t)(GITS_ITT_ENTRY_SIZE - 1U) << GITS_TYPER_ITT_ENTRY_SIZE_SHIFT) |
		((uint64_t)(ARM64_VGIC_LPI_IDBITS - 1U) << GITS_TYPER_IDBITS_SHIFT) |
		((uint64_t)(GITS_DEVBITS - 1U) << GITS_TYPER_DEVBITS_SHIFT);
	its->baser[0U] = vgic_its_baser_type(GITS_BASER_TYPE_DEVICE, 8U);
	its->baser[1U] = vgic_its_baser_type(GITS_BASER_TYPE_COLLECTION, 8U);

	for (idx = 0U; idx < ARM64_VGIC_ITS_COLLECTION_NUM; idx++) {
		its->collection[idx].collection_id = (uint16_t)idx;
		its->collection[idx].target_vcpu = (uint16_t)idx;
		its->collection[idx].valid = (idx < vgic->vcpu_count);
	}
}

static void vgicv3_disarm_vtimer_backup(struct acrn_vcpu *vcpu)
{
	if ((vcpu != NULL) && vcpu->arch.vtimer_backup_initialized) {
		if (timer_is_started(&vcpu->arch.vtimer_backup)) {
			del_timer(&vcpu->arch.vtimer_backup);
		}
		update_timer(&vcpu->arch.vtimer_backup, 0UL, 0UL);
	}
}

/*
 * Offline/shared-pCPU timer flow:
 *
 *   vCPU switched out with an enabled future deadline
 *        -> backup host timer is armed at the guest deadline
 *        -> backup handler injects the same guest timer PPI if still offline
 */
static void vgicv3_arm_vtimer_backup(struct acrn_vcpu *vcpu)
{
	struct arm64_vcpu_guest_ctx *gctx;
	uint64_t deadline;

	if ((vcpu == NULL) || !vcpu->arch.vtimer_backup_initialized) {
		return;
	}

	gctx = &vcpu->arch.gctx;
	deadline = vgic_vtimer_host_deadline(gctx);
	if (!vgic_vtimer_guest_enabled(gctx) || ((int64_t)(deadline - cpu_ticks()) <= 0L)) {
		vgicv3_disarm_vtimer_backup(vcpu);
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

static void vgicv3_arm_vtimer_stuck_rescue(struct acrn_vcpu *vcpu)
{
	uint64_t deadline;

	if ((vcpu == NULL) || !vcpu->arch.vtimer_backup_initialized) {
		return;
	}

	if (timer_is_started(&vcpu->arch.vtimer_backup)) {
		return;
	}

	/*
	 * Do not unmask live CNTV or rewrite the LR here. This is only a software
	 * nudge so the current vCPU re-enters the normal sync/flush request path.
	 */
	deadline = cpu_ticks() + us_to_ticks(VGIC_VTIMER_STUCK_RESCUE_US);
	update_timer(&vcpu->arch.vtimer_backup, deadline, 0UL);
	if (add_timer(&vcpu->arch.vtimer_backup) != 0) {
		update_timer(&vcpu->arch.vtimer_backup, 0UL, 0UL);
	}
}

static void vgicv3_arm_vtimer_wfi_rescue(struct acrn_vcpu *vcpu)
{
	vcpu->arch.vtimer_wfi_rescue = true;
	vcpu->arch.gctx.hcr_el2 |= HCR_TWI;
	if (get_running_vcpu(get_pcpu_id()) == vcpu) {
		write_hcr_el2(vcpu->arch.gctx.hcr_el2);
	}
}

static void vgicv3_vtimer_backup_handler(void *data)
{
	struct acrn_vcpu *vcpu = (struct acrn_vcpu *)data;
	struct arm64_vcpu_guest_ctx *gctx;
	uint32_t ctl;
	uint32_t virq;

	if ((vcpu == NULL) || (vcpu->vm == NULL) || (vcpu->state != VCPU_RUNNING)) {
		return;
	}
	if (get_running_vcpu(get_pcpu_id()) == vcpu) {
		struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
		uint64_t flags;
		bool stuck;

		spinlock_irqsave_obtain(&vgic->lock, &flags);
		stuck = vgicv3_vtimer_live_stuck(vcpu);
		spinlock_irqrestore_release(&vgic->lock, flags);
		if (!stuck) {
			return;
		}
		gctx = &vcpu->arch.gctx;
		virq = (gctx->timer_virq == 0U) ? ARM64_GIC_PPI_VIRTUAL_TIMER : gctx->timer_virq;
		ctl = vgic_vtimer_guest_ctl(gctx);
		arm64_vcpu_trace_vtimer(vcpu, ARM64_VTIMER_TRACE_BACKUP, virq,
			ctl, vgic_vtimer_guest_cval(gctx), false, false);
		vgicv3_arm_vtimer_wfi_rescue(vcpu);
		vcpu_make_request(vcpu, ARM64_VCPU_REQUEST_EVENT);
		signal_event(&vcpu->events[ARM64_VCPU_EVENT_VIRTUAL_INTERRUPT]);
		return;
	}

	gctx = &vcpu->arch.gctx;
	virq = (gctx->timer_virq == 0U) ? ARM64_GIC_PPI_VIRTUAL_TIMER : gctx->timer_virq;
	ctl = vgic_vtimer_guest_ctl(gctx);
	if (!vgic_vtimer_guest_expired(gctx)) {
		return;
	}

	arm64_vcpu_trace_vtimer(vcpu, ARM64_VTIMER_TRACE_BACKUP, virq,
		ctl, vgic_vtimer_guest_cval(gctx), false, false);
	(void)vgicv3_inject_current_timer(vcpu, virq, ctl, vgic_vtimer_guest_cval(gctx));
}

static struct acrn_vcpu *vgic_irq_target_vcpu(struct acrn_vcpu *vcpu,
	const struct arm64_vgicv3 *vgic, uint32_t virq)
{
	struct acrn_vcpu *target_vcpu = vcpu;

	if (vgic_irq_is_spi(virq)) {
		uint16_t target_vcpu_id = vgic->irq[0U][virq].target_vcpu;

		if (target_vcpu_id >= vcpu->vm->hw.created_vcpus) {
			target_vcpu_id = 0U;
		}
		target_vcpu = vcpu_from_vid(vcpu->vm, target_vcpu_id);
	}

	return target_vcpu;
}

static uint64_t make_lr_state(const struct arm64_vgic_irq *desc, bool pending, bool active)
{
	uint64_t lr = desc->virq & ICH_LR_VINTID_MASK;
	uint64_t state;

	/*
	 * The LR state encodes the guest-visible lifecycle. Edge interrupts are
	 * pending-only until accepted; level interrupts become active after being
	 * presented so the maintenance interrupt can later reconcile completion.
	 */
	if (active) {
		state = pending ? ICH_LR_STATE_ACTIVE_PENDING : ICH_LR_STATE_ACTIVE;
	} else {
		state = pending ? ICH_LR_STATE_PENDING : ICH_LR_STATE_INVALID;
	}

	lr |= ((uint64_t)desc->priority << ICH_LR_PRIORITY_SHIFT);
	lr |= state << ICH_LR_STATE_SHIFT;
	lr |= ICH_LR_GROUP1;

	if (desc->hw) {
		lr |= ((uint64_t)desc->pirq << ICH_LR_PINTID_SHIFT);
		lr |= ICH_LR_HW;
	} else if (active || !desc->level) {
		/*
		 * Software edge IRQs must report completion even when they start as
		 * pending-only LRs. Linux can wake from WFI with PSTATE.I still set;
		 * the SGI must remain pending until EL1 actually acknowledges and EOIs
		 * it after local_irq_enable().
		 */
		lr |= ICH_LR_EOI;
	}

	return lr;
}

static bool vgic_irq_deliverable(const struct arm64_vgicv3 *vgic,
	const struct acrn_vcpu *vcpu, const struct arm64_vgic_irq *desc)
{
	bool enabled = ((vgic->gicd_ctlr & VGIC_CTLR_ENABLE_G1) != 0U) &&
		((vcpu->arch.vgic.vmcr & ICH_VMCR_VENG1) != 0UL) && desc->enabled;

	if (enabled && vgic_irq_is_lpi(desc->virq)) {
		enabled = (vcpu->vcpu_id < ARM64_VGIC_MAX_VCPUS) &&
			((vgic->gicr_ctlr[vcpu->vcpu_id] & VGICR_CTLR_ENABLE_LPIS) != 0U);
	}

	return enabled;
}

static uint64_t make_lr(const struct arm64_vgic_irq *desc)
{
	return make_lr_state(desc, desc->pending, desc->active);
}

static bool vgicv3_force_vtimer_active_lr(const struct acrn_vcpu *vcpu,
	const struct arm64_vgic_irq *desc)
{
	return vcpu->arch.vtimer_lr_rescue && vgic_is_vtimer_irq(vcpu, desc->virq) &&
		desc->level && desc->pending && !desc->active &&
		vcpu->arch.gctx.cntv_el2_masked &&
		vgic_vtimer_guest_expired(&vcpu->arch.gctx);
}

static uint64_t make_vtimer_lr(const struct acrn_vcpu *vcpu,
	const struct arm64_vgic_irq *desc)
{
	if (vgicv3_force_vtimer_active_lr(vcpu, desc)) {
		return make_lr_state(desc, true, true);
	}

	return make_lr(desc);
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
		if ((lr_state(vcpu->arch.vgic.lr[idx]) != ICH_LR_STATE_INVALID) &&
			(lr_vintid(vcpu->arch.vgic.lr[idx]) == virq)) {
			return (int32_t)idx;
		}
	}

	return -1;
}

static bool vgicv3_vtimer_pending_only_lr(const struct acrn_vcpu *vcpu, uint32_t virq)
{
	int32_t lr_idx = find_lr_for_virq(vcpu, virq);

	if (lr_idx >= 0) {
		uint32_t state = lr_state(vcpu->arch.vgic.lr[(uint32_t)lr_idx]);

		return ((state & ICH_LR_STATE_PENDING) != 0U) &&
			((state & ICH_LR_STATE_ACTIVE) == 0U);
	}

	return false;
}

static bool vgicv3_vtimer_live_stuck(struct acrn_vcpu *vcpu)
{
	const struct arm64_vcpu_guest_ctx *gctx = &vcpu->arch.gctx;
	const struct arm64_vgic_irq *desc;
	uint32_t virq;

	if (!gctx->cntv_el2_masked || !vgic_vtimer_guest_expired(gctx)) {
		return false;
	}

	virq = (gctx->timer_virq == 0U) ? ARM64_GIC_PPI_VIRTUAL_TIMER : gctx->timer_virq;
	desc = vgic_irq_desc(vcpu, virq);

	return (desc != NULL) && desc->level && desc->pending && !desc->active &&
		vgicv3_vtimer_pending_only_lr(vcpu, virq);
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

static void vgic_update_irq_lr(struct acrn_vcpu *vcpu, const struct arm64_vgic_irq *desc)
{
	int32_t lr_idx = find_lr_for_virq(vcpu, desc->virq);

	if (lr_idx >= 0) {
		bool lr_pending = desc->enabled && desc->pending;
		bool lr_active = desc->active;

		if (lr_pending || lr_active) {
			vcpu->arch.vgic.lr[lr_idx] = make_lr_state(desc, lr_pending, lr_active);
		} else {
			remove_lr(vcpu, (uint32_t)lr_idx);
		}
	}
}

static void vgic_update_irq_row_lr(struct acrn_vm *vm, uint16_t vcpu_id,
	const struct arm64_vgic_irq *desc)
{
	struct acrn_vcpu *target_vcpu;

	if (vcpu_id < vm->hw.created_vcpus) {
		target_vcpu = vcpu_from_vid(vm, vcpu_id);
		if (target_vcpu != NULL) {
			vgic_update_irq_lr(target_vcpu, desc);
		}
	}
}

static uint16_t vgic_valid_target_vcpu(const struct arm64_vgicv3 *vgic, uint16_t target_vcpu_id)
{
	return (target_vcpu_id < vgic->vcpu_count) ? target_vcpu_id : 0U;
}

static void vgic_set_spi_target(struct acrn_vm *vm, uint32_t virq, uint16_t target_vcpu_id)
{
	struct arm64_vgicv3 *vgic = &vm->arch_vm.vgic;
	uint16_t old_target;
	uint16_t new_target;
	uint16_t vcpu_id;

	if (!vgic_irq_is_spi(virq)) {
		return;
	}

	old_target = vgic_valid_target_vcpu(vgic, vgic->irq[0U][virq].target_vcpu);
	new_target = vgic_valid_target_vcpu(vgic, target_vcpu_id);
	if (old_target != new_target) {
		struct arm64_vgic_irq *old_desc = &vgic->irq[old_target][virq];
		struct arm64_vgic_irq *new_desc = &vgic->irq[new_target][virq];

		if (old_desc->pending) {
			vgic_set_pending(vgic, old_target, old_desc, false);
			vgic_set_pending(vgic, new_target, new_desc, true);
		}
		if (old_desc->active) {
			old_desc->active = false;
			new_desc->active = true;
		}
		vgic_update_irq_row_lr(vm, old_target, old_desc);
		vgic_update_irq_row_lr(vm, new_target, new_desc);
	}

	for (vcpu_id = 0U; vcpu_id < vgic->vcpu_count; vcpu_id++) {
		vgic->irq[vcpu_id][virq].target_vcpu = (uint8_t)new_target;
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

static uint64_t vgicv3_control_hcr(uint64_t hcr)
{
	/*
	 * ICH_HCR.EOIcount is a hardware-reported completion count. Replaying it
	 * as saved control state can preserve stale EOI evidence across flushes, so
	 * only the actual control bits are written back to the virtual CPU
	 * interface.
	 */
	return (hcr & ~ICH_HCR_EOICOUNT_MASK) | ICH_HCR_EN;
}

static void vgicv3_write_cpuif(const struct arm64_vgicv3_vcpu_ctx *ctx)
{
	write_ich_vmcr_el2(ctx->vmcr);
	write_ich_ap0r0_el2(ctx->ap0r0);
	write_ich_ap1r0_el2(ctx->ap1r0);
	vgicv3_write_lrs(ctx);
	write_ich_hcr_el2(vgicv3_control_hcr(ctx->hcr));
}

static void vgicv3_write_lrs_hcr(const struct arm64_vgicv3_vcpu_ctx *ctx)
{
	/*
	 * Runtime flushes only publish pending virtual interrupts and maintenance
	 * controls. VMCR/AP are guest CPU-interface state; replaying old snapshots
	 * while the vCPU is running can resurrect stale active-priority state.
	 */
	vgicv3_write_lrs(ctx);
	write_ich_hcr_el2(vgicv3_control_hcr(ctx->hcr));
}

static void vgicv3_write_cpuif_control(const struct arm64_vgicv3_vcpu_ctx *ctx)
{
	/*
	 * Trapped ICC control writes update VMCR/SRE immediately for the running
	 * vCPU. This keeps guest priority/group enables live without replaying AP
	 * state, which is owned by the hardware interrupt acknowledge/EOI path.
	 */
	write_icc_sre_el1(ctx->sre | ICC_SRE_SRE);
	write_ich_vmcr_el2(ctx->vmcr);
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

static bool vgicv3_vcpu_is_running_remote(const struct acrn_vcpu *vcpu)
{
	uint16_t pcpu_id = pcpuid_from_vcpu(vcpu);

	return (pcpu_id != get_pcpu_id()) && (get_running_vcpu(pcpu_id) == vcpu);
}

static void vgicv3_record_cpuif_snapshot(struct acrn_vcpu *vcpu,
	struct arm64_vcpu_last_vgic *last, uint32_t source)
{
	uint32_t count = last->count + 1U;

	/*
	 * ICH_MISR/EISR/ELRSR are the hardware-visible maintenance contract.
	 * Recording them before software consumes LRs preserves the evidence that
	 * explains why EL2 was entered and whether EOI state was available.
	 */
	last->tsc = cpu_ticks();
	last->misr = read_ich_misr_el2();
	last->eisr = read_ich_eisr_el2();
	last->elrsr = read_ich_elrsr_el2();
	last->hcr = read_ich_hcr_el2();
	last->vmcr = read_ich_vmcr_el2();
	last->ap0r0 = read_ich_ap0r0_el2();
	last->ap1r0 = read_ich_ap1r0_el2();
	last->lr0 = (vgic_lr_count > 0U) ? read_ich_lr_el2(0U) : 0UL;
	last->lr1 = (vgic_lr_count > 1U) ? read_ich_lr_el2(1U) : 0UL;
	last->source = source;
	last->count = count;
	last->used_lrs = vcpu->arch.vgic.used_lrs;
}

static void vgicv3_read_loaded_lrs(struct acrn_vcpu *vcpu, bool is_current)
{
	if (is_current || vgicv3_vcpu_is_loaded(vcpu)) {
		vgicv3_read_lrs(&vcpu->arch.vgic);
	}
}

static bool vgicv3_lrs_changed(const struct arm64_vgicv3_vcpu_ctx *ctx,
	const uint64_t old_lrs[ARM64_VGIC_MAX_LRS], uint8_t old_used_lrs, uint64_t old_hcr)
{
	bool changed = (ctx->used_lrs != old_used_lrs) || (ctx->hcr != old_hcr);
	uint32_t idx;

	for (idx = 0U; !changed && (idx < ARM64_VGIC_MAX_LRS); idx++) {
		changed = (ctx->lr[idx] != old_lrs[idx]);
	}

	return changed;
}

static void vgicv3_complete_eoi_lrs(struct acrn_vcpu *vcpu, uint64_t eoi_lrs,
	const uint64_t *old_lrs, uint8_t old_used_lrs)
{
	struct arm64_vgicv3_vcpu_ctx *ctx = &vcpu->arch.vgic;
	uint32_t idx = min_u32(vgic_lr_count, ARM64_VGIC_MAX_LRS);

	while (idx > 0U) {
		uint32_t lr_idx = idx - 1U;
		uint64_t lr;
		uint32_t virq;
		uint32_t state;
		struct arm64_vgic_irq *desc;

		idx--;
		if ((eoi_lrs & (1UL << lr_idx)) == 0UL) {
			continue;
		}

		lr = (lr_idx < ctx->used_lrs) ? ctx->lr[lr_idx] : 0UL;
		state = lr_state(lr);
		if ((state == ICH_LR_STATE_INVALID) &&
			(old_lrs != NULL) && (lr_idx < old_used_lrs)) {
			/*
			 * EISR is an event bitmap, while LR readback is stateful. Some
			 * implementations can report the EOI after the LR slot has already
			 * become invalid, so use the pre-sync software LR as the identity
			 * source to clear the matching virtual IRQ active state.
			 */
			lr = old_lrs[lr_idx];
			state = lr_state(lr);
		}
		if (state == ICH_LR_STATE_INVALID) {
			continue;
		}

		virq = lr_vintid(lr);
		desc = vgic_irq_desc(vcpu, virq);
		if (desc != NULL) {
			bool lr_pending = ((state & ICH_LR_STATE_PENDING) != 0U);
			bool lr_active = ((state & ICH_LR_STATE_ACTIVE) != 0U);
			bool keep_pending = desc->level ?
				(lr_pending || desc->pending) : (lr_pending && lr_active);

			/*
			 * The virtual timer is level-triggered by CNTV, not by the
			 * cached pending bit alone. On EOI maintenance, the guest may
			 * already have moved CNTV_CVAL to a future deadline without an
			 * intervening EL2 sysreg trap, so resample CNTV before deciding
			 * whether to requeue the timer LR.
			 */
			if (vgic_is_vtimer_irq(vcpu, virq) && vgicv3_vcpu_is_loaded(vcpu)) {
				/*
				 * The timer source is the live CNTV line. Once the guest
				 * completes a timer LR, immediately resample the line. If the
				 * deadline is still due, keep the virtual line asserted instead
				 * of depending on another host PPI edge to recreate it.
				 */
				keep_pending = vgic_sample_current_vtimer(vcpu);
				vcpu->arch.vtimer_lr_rescue = false;
			}

			vgic_set_pending(&vcpu->vm->arch_vm.vgic, vcpu->vcpu_id, desc, keep_pending);
			desc->active = false;
			if (vgic_is_vtimer_irq(vcpu, virq)) {
				vgic_timer_set_host_mask(vcpu, keep_pending);
			}
		}
		if (lr_idx < ctx->used_lrs) {
			remove_lr(vcpu, lr_idx);
		}
	}
}

static void vgicv3_deactivate_irq_locked(struct acrn_vcpu *vcpu, uint32_t virq)
{
	struct arm64_vgic_irq *desc;
	int32_t lr_idx;
	bool keep_pending = false;

	if (virq >= ARM64_VGIC_IRQ_NUM) {
		return;
	}

	/*
	 * ICC_DIR_EL1 is the explicit deactivation path when a guest uses split
	 * priority-drop/deactivate semantics. Treating a DIR write as idempotent
	 * is safe for the current combined-EOI model and prevents a guest-visible
	 * active LR from sticking if Linux takes the split path anyway.
	 */
	vgicv3_sync_vcpu(vcpu, true);
	desc = vgic_irq_desc(vcpu, virq);
	if (desc == NULL) {
		return;
	}

	if (vgic_is_vtimer_irq(vcpu, virq) && vgicv3_vcpu_is_loaded(vcpu)) {
		keep_pending = vgic_sample_current_vtimer(vcpu);
	} else {
		keep_pending = desc->level && desc->pending;
	}

	lr_idx = find_lr_for_virq(vcpu, virq);
	if (lr_idx >= 0) {
		remove_lr(vcpu, (uint32_t)lr_idx);
	}
	desc->active = false;
	vgic_set_pending(&vcpu->vm->arch_vm.vgic, vcpu->vcpu_id, desc, keep_pending);
	if (vgic_is_vtimer_irq(vcpu, virq)) {
		vgic_timer_set_host_mask(vcpu, keep_pending);
	}
}

static void vgicv3_write_loaded_lrs(const struct acrn_vcpu *vcpu, bool is_current)
{
	if (is_current || vgicv3_vcpu_is_loaded(vcpu)) {
		vgicv3_write_lrs_hcr(&vcpu->arch.vgic);
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

void arm64_vgicv3_init_vm(struct acrn_vm *vm, uint64_t cpu_affinity)
{
	struct arm64_vgicv3 *vgic = &vm->arch_vm.vgic;
	uint32_t idx;
	uint32_t spi;
	uint16_t vcpu_count = arm64_vcpu_count_from_affinity(cpu_affinity);

	/*
	 * The virtual distributor advertises a compact GICv3 with affinity routing
	 * and Group-1 interrupts enabled. Guest redistributor frames are indexed by
	 * guest vCPU ID, so guest MPIDR affinity and GICR_TYPER affinity stay in
	 * the same compact namespace even when the VM runs on sparse pCPU IDs.
	 */
	arm64_vgicv3_global_init();

	(void)memset(vgic, 0U, sizeof(*vgic));
	spinlock_init(&vgic->lock);
	vgic->vcpu_count = (uint16_t)min_u32((uint32_t)vcpu_count, ARM64_VGIC_MAX_VCPUS);
	if (vgic->vcpu_count == 0U) {
		vgic->vcpu_count = 1U;
	}
	vgic->rdist_count = vgic->vcpu_count;
	vgic->lr_count = vgic_lr_count;
	vgic->vmcr = (uint32_t)(VGIC_VMCR_GROUP_ENABLES | ICH_VMCR_DEFAULT_MASK);
	vgic->gicd_ctlr = VGIC_CTLR_ARE_NS | VGIC_CTLR_ENABLE_G1;
	vgic->its_enabled = (arm64_platform_guest_its_size(vm->vm_id) != 0UL);
	vgic->gicd_typer = (((ARM64_VGIC_IRQ_NUM / 32U) - 1U) & 0x1fU) |
		(((uint32_t)(vgic->vcpu_count - 1U) & 0x7U) << 5U);
	if (vgic->its_enabled) {
		vgic->gicd_typer |= (12U << VGICD_TYPER_NUM_LPIS_SHIFT) |
			VGICD_TYPER_LPIS |
			((ARM64_VGIC_LPI_IDBITS - 1U) << VGICD_TYPER_IDBITS_SHIFT);
	}
	vgic->gicd_pidr2 = GIC_PIDR2_ARCH_GICV3;

	for (idx = 0U; idx < ARM64_VGIC_MAX_VCPUS; idx++) {
		uint32_t virq;

		vgic->rdist_vcpu[idx] = VGIC_INVALID_VCPU_ID;
		for (virq = 0U; virq < ARM64_VGIC_IRQ_NUM; virq++) {
			struct arm64_vgic_irq *desc = &vgic->irq[idx][virq];

			desc->virq = (uint16_t)virq;
			desc->pirq = 0xffffU;
			desc->priority = GIC_PRIORITY_DEFAULT;
			desc->target_vcpu = (virq < ARM64_VGIC_LOCAL_IRQ_NUM) ? (uint8_t)idx : 0U;
			desc->level = (virq >= ARM64_VGIC_SGI_NUM);
			desc->group1 = true;
			desc->groupmod = false;
		}
		for (virq = 0U; virq < ARM64_VGIC_LPI_NUM; virq++) {
			struct arm64_vgic_irq *desc = &vgic->lpi[idx][virq];

			desc->virq = (uint16_t)(ARM64_VGIC_LPI_BASE + virq);
			desc->pirq = 0xffffU;
			desc->priority = GIC_PRIORITY_DEFAULT;
			desc->target_vcpu = (uint8_t)idx;
			desc->enabled = true;
			desc->level = false;
			desc->group1 = true;
			desc->groupmod = false;
		}
	}

	for (spi = 0U; spi < ARM64_VGIC_SPI_NUM; spi++) {
		vgic->spi_router[spi] = 0UL;
	}

	if (vgic->its_enabled) {
		vgic_its_init(vgic);
	}
	vgic->initialized = true;
}

void arm64_vgicv3_init_vcpu(struct acrn_vcpu *vcpu)
{
	struct arm64_vgicv3_vcpu_ctx *ctx = &vcpu->arch.vgic;
	struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
	uint16_t rdist_id = vcpu->vcpu_id;

	(void)memset(ctx, 0U, sizeof(*ctx));
	/*
	 * TC traps common ICC registers, including SGI generation and EOImode
	 * control, so the software vGIC owns the interrupt lifecycle. Leaving these
	 * accesses virtual in hardware would let Linux enable split deactivate
	 * semantics that this vGIC does not yet model.
	 */
	ctx->hcr = ICH_HCR_EN | ICH_HCR_TC;
	ctx->vmcr = VGIC_VMCR_GROUP_ENABLES | ICH_VMCR_DEFAULT_MASK;
	ctx->sre = ICC_SRE_SRE;
	ctx->pmr = GIC_PRIORITY_LOWEST;
	ctx->ctlr = 0UL;
	initialize_timer(&vcpu->arch.vtimer_backup, vgicv3_vtimer_backup_handler,
		vcpu, 0UL, 0UL);
	vcpu->arch.vtimer_backup_initialized = true;

	if ((rdist_id < ARM64_VGIC_MAX_VCPUS) && (vcpu->vcpu_id < vgic->vcpu_count)) {
		vgic->rdist_vcpu[rdist_id] = vcpu->vcpu_id;
	}
}

void arm64_vgicv3_arm_vtimer_backup(struct acrn_vcpu *vcpu)
{
	vgicv3_arm_vtimer_backup(vcpu);
}

void arm64_vgicv3_cancel_vtimer_backup(struct acrn_vcpu *vcpu)
{
	vgicv3_disarm_vtimer_backup(vcpu);
}

int32_t arm64_vgicv3_handle_cpuif_sysreg(struct acrn_vcpu *vcpu, uint32_t sysreg,
	bool read, uint64_t *reg)
{
	struct arm64_vgicv3 *vgic;
	struct arm64_vgicv3_vcpu_ctx *ctx;
	struct arm64_vcpu_last_cpuif *last;
	uint64_t flags;
	uint64_t value = 0UL;
	uint64_t access_value;
	bool control_changed = false;
	bool flush_needed = false;
	int32_t ret = 0;

	if ((vcpu == NULL) || (vcpu->vm == NULL) || (reg == NULL)) {
		return -EINVAL;
	}

	vgic = &vcpu->vm->arch_vm.vgic;
	ctx = &vcpu->arch.vgic;
	last = &vcpu->arch.debug.last_cpuif;
	access_value = read ? 0UL : *reg;
	spinlock_irqsave_obtain(&vgic->lock, &flags);
	switch (sysreg) {
	case ARM64_VGIC_SYSREG_ICC_SRE_EL1:
		if (read) {
			value = ctx->sre | ICC_SRE_SRE;
		} else {
			ctx->sre = *reg | ICC_SRE_SRE;
			control_changed = true;
		}
		break;
	case ARM64_VGIC_SYSREG_ICC_PMR_EL1:
		if (read) {
			value = ctx->pmr;
		} else {
			ctx->pmr = *reg & 0xffUL;
			ctx->vmcr = (ctx->vmcr & ~(0xffUL << 24U)) | (ctx->pmr << 24U);
			control_changed = true;
		}
		break;
	case ARM64_VGIC_SYSREG_ICC_CTLR_EL1:
		if (read) {
			value = ctx->ctlr;
		} else {
			/*
			 * The current LR lifecycle completes software-backed virtual IRQs
			 * through EOIR maintenance. Keep EOImode as combined priority-drop
			 * plus deactivate until ICC_DIR_EL1 deactivation is fully modeled.
			 */
			ctx->ctlr = 0UL;
			ctx->vmcr &= ~ICH_VMCR_EOIM;
			control_changed = true;
		}
		break;
	case ARM64_VGIC_SYSREG_ICC_IGRPEN1_EL1:
		if (read) {
			value = ((ctx->vmcr & ICH_VMCR_VENG1) != 0UL) ? 1UL : 0UL;
		} else if ((*reg & 1UL) != 0UL) {
			ctx->vmcr |= ICH_VMCR_VENG1;
			control_changed = true;
		} else {
			ctx->vmcr &= ~ICH_VMCR_VENG1;
			control_changed = true;
		}
		break;
	case ARM64_VGIC_SYSREG_ICC_DIR_EL1:
		/*
		 * ICC_DIR_EL1 carries the INTID to deactivate. Linux should only need
		 * it when EOImode is split, but honoring the write is harmless for the
		 * combined model and prevents a stale software active bit if the guest
		 * still takes the explicit deactivate path.
		 */
		if (read) {
			value = 0UL;
		} else {
			vgicv3_deactivate_irq_locked(vcpu, (uint32_t)(*reg & ICH_LR_VINTID_MASK));
			flush_needed = true;
		}
		break;
	case ARM64_VGIC_SYSREG_ICC_RPR_EL1:
		/*
		 * RPR is observational state. Returning the idle priority is sufficient
		 * for guests that probe it while virtual IRQ acknowledgement/deactivation
		 * remains in hardware through IAR/EOIR.
		 */
		if (read) {
			value = GIC_PRIORITY_LOWEST;
		}
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (ret == 0) {
		if (read) {
			*reg = value;
			access_value = value;
		}
		/*
		 * Record after emulation so reads capture the returned value and writes
		 * can be matched against the vGIC state produced by the access.
		 */
		last->sysreg = sysreg;
		last->value = access_value;
		last->status = ret;
		last->read = read;
		last->tsc = cpu_ticks();
		if (control_changed && vgicv3_vcpu_is_loaded(vcpu)) {
			vgicv3_write_cpuif_control(ctx);
		}
		if (control_changed || flush_needed) {
			vgicv3_flush_vcpu(vcpu, true);
		}
	}
	spinlock_irqrestore_release(&vgic->lock, flags);

	return ret;
}

void arm64_vgicv3_reset_vcpu(struct acrn_vcpu *vcpu)
{
	arm64_vgicv3_init_vcpu(vcpu);
}

static void vgicv3_clear_vcpu_boot_irqs(struct acrn_vcpu *vcpu)
{
	struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
	uint32_t virq;

	/*
	 * CPU_ON is a vCPU reset boundary. Clear private interrupt delivery state
	 * so a secondary CPU does not enter Linux with stale SGI/PPI/LPI state,
	 * while preserving distributor configuration and any shared SPI state.
	 */
	for (virq = 0U; virq < ARM64_VGIC_LOCAL_IRQ_NUM; virq++) {
		struct arm64_vgic_irq *desc = &vgic->irq[vcpu->vcpu_id][virq];

		vgic_set_pending(vgic, vcpu->vcpu_id, desc, false);
		desc->active = false;
	}
	for (virq = 0U; vgic->its_enabled && (virq < ARM64_VGIC_LPI_NUM); virq++) {
		struct arm64_vgic_irq *desc = &vgic->lpi[vcpu->vcpu_id][virq];

		vgic_set_pending(vgic, vcpu->vcpu_id, desc, false);
		desc->active = false;
	}
}

void arm64_vgicv3_reset_vcpu_boot_state(struct acrn_vcpu *vcpu)
{
	struct arm64_vgicv3 *vgic;
	uint64_t flags;

	if ((vcpu == NULL) || (vcpu->vm == NULL)) {
		return;
	}

	arm64_vgicv3_cancel_vtimer_backup(vcpu);
	arm64_vgicv3_reset_vcpu(vcpu);
	vgic = &vcpu->vm->arch_vm.vgic;
	if (!vgic->initialized || (vcpu->vcpu_id >= ARM64_VGIC_MAX_VCPUS)) {
		return;
	}

	spinlock_irqsave_obtain(&vgic->lock, &flags);
	vgicv3_clear_vcpu_boot_irqs(vcpu);
	spinlock_irqrestore_release(&vgic->lock, flags);
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
	vgicv3_write_cpuif(ctx);
}

void arm64_vgicv3_save_vcpu(struct acrn_vcpu *vcpu)
{
	struct arm64_vgicv3_vcpu_ctx *ctx = &vcpu->arch.vgic;

	/*
	 * EOIcount is consumed from the live CPU interface during sync. Saving it
	 * into the vCPU context would make later loads replay an old completion
	 * count as if it were a persistent HCR control bit.
	 */
	ctx->hcr = read_ich_hcr_el2() & ~ICH_HCR_EOICOUNT_MASK;
	ctx->vmcr = read_ich_vmcr_el2();
	ctx->ap0r0 = read_ich_ap0r0_el2();
	ctx->ap1r0 = read_ich_ap1r0_el2();
	ctx->sre = read_icc_sre_el1();
	ctx->pmr = read_icc_pmr_el1();
	ctx->ctlr = read_icc_ctlr_el1();
	vgicv3_read_lrs(ctx);
	write_ich_hcr_el2(0UL);
}

static bool vgicv3_active_priority_empty(void)
{
	/*
	 * The virtual AP registers track guest active priority state. If both
	 * groups are empty, EL1 has returned from the interrupt handler far enough
	 * that a remaining Active LR is stale state rather than a handler still in
	 * progress.
	 */
	return (read_ich_ap0r0_el2() == 0UL) && (read_ich_ap1r0_el2() == 0UL);
}

static bool vgicv3_complete_timer_lr(struct acrn_vcpu *vcpu,
	struct arm64_vgic_irq *desc, uint32_t lr_idx, bool *removed)
{
	bool line_asserted;

	if (removed != NULL) {
		*removed = false;
	}
	if (!vgicv3_active_priority_empty()) {
		return false;
	}

	/*
	 * CNTV is the level source for the guest timer. Once AP state is empty the
	 * consumed LR no longer owns redelivery; remove it and immediately
	 * resample CNTV so an already-expired deadline stays asserted in the vGIC.
	 */
	line_asserted = vgic_sample_current_vtimer(vcpu);
	desc->active = false;
	vgic_set_pending(&vcpu->vm->arch_vm.vgic, vcpu->vcpu_id, desc, line_asserted);
	if (!line_asserted) {
		vcpu->arch.vtimer_lr_rescue = false;
	}
	remove_lr(vcpu, lr_idx);
	if (removed != NULL) {
		*removed = true;
	}
	vgic_timer_set_host_mask(vcpu, line_asserted);
	arm64_vcpu_trace_vtimer(vcpu, ARM64_VTIMER_TRACE_EOI, desc->virq,
		UINT32_MAX, UINT64_MAX, false, false);

	return true;
}

static bool vgicv3_requeue_lost_masked_timer(struct acrn_vcpu *vcpu)
{
	struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
	struct arm64_vgic_irq *desc;
	uint64_t flags;
	uint32_t virq;
	bool requeued = false;

	if (!vcpu->arch.gctx.cntv_el2_masked) {
		return false;
	}

	virq = vcpu->arch.gctx.timer_virq;
	if (virq == 0U) {
		virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
		vcpu->arch.gctx.timer_virq = virq;
	}

	spinlock_irqsave_obtain(&vgic->lock, &flags);
	vgicv3_sync_vcpu(vcpu, true);
	desc = vgic_irq_desc(vcpu, virq);
	if ((desc != NULL) && !desc->pending && !desc->active &&
		(find_lr_for_virq(vcpu, virq) < 0) && vgic_sample_current_vtimer(vcpu)) {
		/*
		 * EL2 may have masked live CNTV while a timer LR was in flight, then later
		 * observe no LR/descriptor owner after hardware consumed the pending-only
		 * entry. Requeue from the live CNTV level so the next flush has a normal
		 * virtual interrupt to present.
		 */
		vgic_set_pending(vgic, vcpu->vcpu_id, desc, true);
		vgicv3_flush_vcpu(vcpu, true);
		arm64_vcpu_trace_vtimer(vcpu, ARM64_VTIMER_TRACE_REQUEUE, virq,
			UINT32_MAX, UINT64_MAX, false, false);
		requeued = true;
	}
	spinlock_irqrestore_release(&vgic->lock, flags);

	return requeued;
}

static void vgicv3_sync_timer_line_locked(struct acrn_vcpu *vcpu, bool is_current,
	bool line_asserted)
{
	struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
	struct arm64_vgic_irq *desc;
	uint32_t virq = vcpu->arch.gctx.timer_virq;
	int32_t lr_idx;

	if (virq == 0U) {
		virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
		vcpu->arch.gctx.timer_virq = virq;
	}

	desc = vgic_irq_desc(vcpu, virq);
	if (desc == NULL) {
		return;
	}

	/*
	 * The timer interrupt is a level line whose source is CNTV while loaded
	 * and the saved CNTV deadline while offline. Lowering the line must remove
	 * stale pending-only LR state as well as the software pending bit, otherwise
	 * a consumed timer can be presented again after the guest has programmed a
	 * future deadline.
	 */
	desc->level = true;
	vgic_set_pending(vgic, vcpu->vcpu_id, desc, line_asserted);
	if (!line_asserted) {
		vcpu->arch.vtimer_lr_rescue = false;
	}
	lr_idx = find_lr_for_virq(vcpu, virq);
	if (lr_idx >= 0) {
		uint32_t state = lr_state(vcpu->arch.vgic.lr[lr_idx]);
		bool lr_active = ((state & ICH_LR_STATE_ACTIVE) != 0U);

		if (line_asserted || lr_active) {
			vcpu->arch.vgic.lr[lr_idx] = make_lr_state(desc, line_asserted, lr_active);
		} else {
			remove_lr(vcpu, (uint32_t)lr_idx);
		}
	}
	vgic_timer_set_host_mask(vcpu, line_asserted || desc->active);
	if (!line_asserted) {
		if (is_current || vgicv3_vcpu_is_loaded(vcpu)) {
			vgicv3_disarm_vtimer_backup(vcpu);
		} else {
			vgicv3_arm_vtimer_backup(vcpu);
		}
	} else {
		vgicv3_disarm_vtimer_backup(vcpu);
	}
	vgicv3_flush_vcpu(vcpu, is_current);
}

static void vgicv3_sync_vcpu(struct acrn_vcpu *vcpu, bool is_current)
{
	struct arm64_vgicv3_vcpu_ctx *ctx = &vcpu->arch.vgic;
	uint64_t old_lrs[ARM64_VGIC_MAX_LRS];
	uint8_t old_used_lrs;
	uint64_t eoi_lrs = 0UL;
	uint32_t idx = 0U;
	bool loaded;

	if (!is_current && vgicv3_vcpu_is_running_remote(vcpu)) {
		return;
	}
	loaded = is_current || vgicv3_vcpu_is_loaded(vcpu);

	/*
	 * Synchronization is the authoritative readback point from hardware LRs
	 * into the software IRQ descriptors. It is called before changing virtual
	 * IRQ state and from maintenance IRQs when hardware reports LR pressure or
	 * completion.
	 *
	 *   ICH_LR<n>/EISR/EOIcount -> descriptor pending/active bits
	 *        -> compact saved LRs -> next flush can refill free slots
	 */
	old_used_lrs = ctx->used_lrs;
	(void)memcpy(old_lrs, ctx->lr, sizeof(old_lrs));
	vgicv3_read_loaded_lrs(vcpu, is_current);
	if (loaded) {
		uint64_t live_hcr = read_ich_hcr_el2();

		eoi_lrs = read_ich_eisr_el2();
		vgicv3_record_cpuif_snapshot(vcpu, &vcpu->arch.debug.last_vgic_sync,
			ARM64_VCPU_DEBUG_VGIC_SYNC);
		if ((live_hcr & ICH_HCR_EOICOUNT_MASK) != 0UL) {
			/*
			 * EOIcount reports guest EOIs that did not surface through EISR.
			 * Consume the count after recording it so later HCR writes do not
			 * keep rediscovering the same completed interrupt.
			 */
			ctx->hcr = live_hcr & ~ICH_HCR_EOICOUNT_MASK;
			write_ich_hcr_el2(vgicv3_control_hcr(ctx->hcr));
		} else {
			ctx->hcr &= ~ICH_HCR_EOICOUNT_MASK;
		}
	}
	if (((ctx->vmcr & ICH_VMCR_EOIM) == 0UL) && (eoi_lrs != 0UL)) {
		vgicv3_complete_eoi_lrs(vcpu, eoi_lrs, old_lrs, old_used_lrs);
	}

	for (idx = 0U; idx < old_used_lrs; idx++) {
		uint64_t old_lr = old_lrs[idx];
		uint32_t virq;
		struct arm64_vgic_irq *desc;

		if (lr_state(old_lr) == ICH_LR_STATE_INVALID) {
			continue;
		}

		virq = lr_vintid(old_lr);
		if (find_lr_for_virq(vcpu, virq) >= 0) {
			continue;
		}

		desc = vgic_irq_desc(vcpu, virq);
		if (desc != NULL) {
			bool queued_level = desc->level && desc->pending;
			bool old_pending = ((lr_state(old_lr) & ICH_LR_STATE_PENDING) != 0U);
			bool eoi_reported = (idx < BITS_PER_LONG) &&
				((eoi_lrs & (1UL << idx)) != 0UL);
			bool timer_irq = vgic_is_vtimer_irq(vcpu, virq);
			bool timer_line_sampled = false;
			bool timer_line_asserted = false;

			if (queued_level) {
				/* Keep the level source queued for the next flush pass. */
			} else if (loaded && desc->level && timer_irq && old_pending && !eoi_reported) {
				/*
				 * A pending-only timer LR can be consumed as a WFI wake event before
				 * Linux unmasks IRQs and acknowledges PPI27. The architectural source
				 * is still CNTV, so preserve the virtual line when the live timer is
				 * still asserted instead of clearing the descriptor just because the
				 * LR slot became empty without EOI evidence.
				 */
				timer_line_asserted = vgic_sample_current_vtimer(vcpu);
				timer_line_sampled = true;
				vgic_set_pending(&vcpu->vm->arch_vm.vgic, vcpu->vcpu_id,
					desc, timer_line_asserted);
			} else if (!desc->level && old_pending && !eoi_reported) {
				/*
				 * A pending edge LR can wake an idle guest before EL1 has
				 * re-enabled IRQs and acknowledged the SGI. If it disappears
				 * without EOI evidence, treat the edge as not yet consumed.
				 */
				vgic_set_pending(&vcpu->vm->arch_vm.vgic, vcpu->vcpu_id, desc, true);
			} else {
				vgic_set_pending(&vcpu->vm->arch_vm.vgic, vcpu->vcpu_id, desc, false);
			}
			desc->active = false;
			if (timer_irq && !queued_level) {
				vgic_timer_set_host_mask(vcpu,
					timer_line_sampled && timer_line_asserted);
			}
		}
	}

	idx = 0U;
	while (idx < ctx->used_lrs) {
		uint64_t lr = ctx->lr[idx];
		uint32_t virq = lr_vintid(lr);
		uint32_t state = lr_state(lr);
		struct arm64_vgic_irq *desc = vgic_irq_desc(vcpu, virq);

		if (desc != NULL) {
			bool lr_pending = ((state & ICH_LR_STATE_PENDING) != 0U);
			bool lr_active = ((state & ICH_LR_STATE_ACTIVE) != 0U);
			bool removed = false;

			if (!desc->level && lr_active && !lr_pending) {
				/*
				 * Software edge IRQs remain active until an EOI maintenance
				 * event proves that the guest completed the interrupt. Clearing
				 * active-only SGIs here can lose an IPI that woke Linux from
				 * idle before local_irq_enable() ran the handler.
				 */
				desc->active = true;
				idx++;
				continue;
			}
			if (loaded && desc->level && vgic_is_vtimer_irq(vcpu, virq) &&
				lr_active && (!vcpu->arch.vtimer_lr_rescue || !lr_pending) &&
				vgicv3_complete_timer_lr(vcpu, desc, idx, &removed)) {
				if (!removed) {
					idx++;
				}
				continue;
			}
			if (loaded && desc->level && vgic_is_vtimer_irq(vcpu, virq) &&
				lr_pending && !lr_active) {
				bool line_asserted = vgic_sample_current_vtimer(vcpu);

				/*
				 * QEMU can consume a pending-only timer LR as a WFI wake event
				 * before Linux unmasks IRQs and acknowledges PPI27. Keep the
				 * software level line asserted while CNTV is still due; otherwise
				 * EL2 can be left with a host-masked expired CNTV source and no
				 * descriptor state from which to rebuild delivery.
				 */
				vgic_set_pending(&vcpu->vm->arch_vm.vgic, vcpu->vcpu_id,
					desc, line_asserted);
				desc->active = false;
				vgic_timer_set_host_mask(vcpu, line_asserted);
				if (!line_asserted) {
					remove_lr(vcpu, idx);
				} else {
					idx++;
				}
				continue;
			}

			/*
			 * LR pending is already resident in the virtual CPU interface.
			 * Mirroring it back into the software pending bitmap makes flush
			 * continuously rebuild the same active+pending LR, so keep the
			 * bitmap for IRQs that still need an LR slot.
			 */
			if (lr_pending || !lr_active || !desc->level || !desc->pending) {
				vgic_set_pending(&vcpu->vm->arch_vm.vgic, vcpu->vcpu_id, desc, false);
			}
			desc->active = lr_active;
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
	if ((vcpu != NULL) && (vcpu->vm != NULL) && vcpu->vm->arch_vm.vgic.initialized) {
		vgicv3_sync_vcpu(vcpu, false);
	}
}

void arm64_vgicv3_sync_current_vcpu(struct acrn_vcpu *vcpu)
{
	if ((vcpu != NULL) && (vcpu->vm != NULL) && vcpu->vm->arch_vm.vgic.initialized) {
		vgicv3_sync_vcpu(vcpu, true);
	}
}

void arm64_vgicv3_complete_wfi_irqs(struct acrn_vcpu *vcpu)
{
	struct arm64_vgicv3 *vgic;
	struct arm64_vgicv3_vcpu_ctx *ctx;
	uint64_t flags;
	uint32_t idx = 0U;

	if ((vcpu == NULL) || (vcpu->vm == NULL) || !vcpu->vm->arch_vm.vgic.initialized) {
		return;
	}

	vgic = &vcpu->vm->arch_vm.vgic;
	ctx = &vcpu->arch.vgic;
	spinlock_irqsave_obtain(&vgic->lock, &flags);
	vgicv3_sync_vcpu(vcpu, true);
	while (idx < ctx->used_lrs) {
		uint64_t lr = ctx->lr[idx];
		uint32_t state = lr_state(lr);
		uint32_t virq = lr_vintid(lr);
		struct arm64_vgic_irq *desc = vgic_irq_desc(vcpu, virq);
		bool removed = false;

		if ((desc != NULL) && !desc->level && (state == ICH_LR_STATE_ACTIVE)) {
			/*
			 * SGIs are edge-triggered software IRQs. If Linux reaches WFI with
			 * an SGI LR still active-only, the interrupt handler has returned
			 * far enough that keeping the LR active only blocks later IPIs; a
			 * pending bit would have kept it deliverable instead.
			 */
			desc->active = false;
			remove_lr(vcpu, idx);
			continue;
		}
		if ((desc != NULL) && desc->level && vgic_is_vtimer_irq(vcpu, virq) &&
			((state & ICH_LR_STATE_ACTIVE) != 0U) &&
			(!vcpu->arch.vtimer_lr_rescue ||
				((state & ICH_LR_STATE_PENDING) == 0U)) &&
			vgicv3_complete_timer_lr(vcpu, desc, idx, &removed)) {
			if (!removed) {
				idx++;
			}
			continue;
		}
		idx++;
	}
	vgicv3_flush_vcpu(vcpu, true);
	spinlock_irqrestore_release(&vgic->lock, flags);
}

bool arm64_vgicv3_has_pending_irq(struct acrn_vcpu *vcpu)
{
	struct arm64_vgicv3 *vgic;
	struct arm64_vgicv3_vcpu_ctx *ctx;
	uint64_t flags;
	uint32_t idx;
	uint32_t word;
	bool pending = false;

	if ((vcpu == NULL) || (vcpu->vm == NULL) || !vcpu->vm->arch_vm.vgic.initialized) {
		return false;
	}

	vgic = &vcpu->vm->arch_vm.vgic;
	ctx = &vcpu->arch.vgic;
	spinlock_irqsave_obtain(&vgic->lock, &flags);
	/*
	 * WFI must not sleep when a virtual interrupt is already visible through
	 * a list register. Active+Pending carries a redelivery after guest EOI, so
	 * treat any LR pending bit as enough work to re-enter EL1 immediately.
	 */
	for (idx = 0U; idx < ctx->used_lrs; idx++) {
		if ((lr_state(ctx->lr[idx]) & ICH_LR_STATE_PENDING) != 0U) {
			pending = true;
			break;
		}
	}

	/*
	 * Software pending bits are not enough by themselves; the distributor,
	 * virtual CPU interface, and interrupt enable state must all allow
	 * delivery before WFI should behave as if an interrupt can wake it now.
	 */
	for (word = 0U; !pending && (word < ARM64_VGIC_WORDS); word++) {
		uint32_t bits = vgic->pending_bitmap[vcpu->vcpu_id][word];

		while (bits != 0U) {
			uint32_t bit = (uint32_t)ffs64((uint64_t)bits);
			uint32_t virq = (word * 32U) + bit;
			struct arm64_vgic_irq *desc = &vgic->irq[vcpu->vcpu_id][virq];

			bits &= ~BIT32(bit);
			if (desc->pending && vgic_irq_deliverable(vgic, vcpu, desc)) {
				pending = true;
				break;
			}
		}
	}
	for (word = 0U; vgic->its_enabled && !pending && (word < ARM64_VGIC_LPI_WORDS); word++) {
		uint32_t bits = vgic->lpi_pending_bitmap[vcpu->vcpu_id][word];

		while (bits != 0U) {
			uint32_t bit = (uint32_t)ffs64((uint64_t)bits);
			uint32_t lpi = ARM64_VGIC_LPI_BASE + (word * 32U) + bit;
			struct arm64_vgic_irq *desc = &vgic->lpi[vcpu->vcpu_id][lpi - ARM64_VGIC_LPI_BASE];

			bits &= ~BIT32(bit);
			if (desc->pending && vgic_irq_deliverable(vgic, vcpu, desc)) {
				pending = true;
				break;
			}
		}
	}
	spinlock_irqrestore_release(&vgic->lock, flags);

	return pending;
}

static void vgicv3_flush_vcpu(struct acrn_vcpu *vcpu, bool is_current)
{
	struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
	struct arm64_vgicv3_vcpu_ctx *ctx = &vcpu->arch.vgic;
	uint32_t word;

	if (!is_current && vgicv3_vcpu_is_running_remote(vcpu)) {
		return;
	}

	/*
	 * Flush chooses pending, enabled virtual IRQs and materializes them into
	 * hardware LRs. If no LR is available, underflow/maintenance signaling is
	 * enabled so the guest exit path can retry after the guest consumes entries.
	 * The pending bitmap avoids scanning the full virtual INTID space when the
	 * common case has only a few pending interrupts.
	 *
	 *   pending bitmap -> deliverability check -> LR slot
	 *        -> consume software pending state or set UIE when LRs are full
	 */
	ctx->hcr &= ~ICH_HCR_UIE;

	for (word = 0U; word < ARM64_VGIC_WORDS; word++) {
		uint32_t bits = vgic->pending_bitmap[vcpu->vcpu_id][word];

		while (bits != 0U) {
			uint32_t bit = (uint32_t)ffs64((uint64_t)bits);
			uint32_t virq = (word * 32U) + bit;
			struct arm64_vgic_irq *desc = &vgic->irq[vcpu->vcpu_id][virq];
			int32_t lr_idx;

			bits &= ~BIT32(bit);
			if (!desc->pending) {
				vgic_set_pending(vgic, vcpu->vcpu_id, desc, false);
				continue;
			}
			if (!vgic_irq_deliverable(vgic, vcpu, desc)) {
				continue;
			}

			lr_idx = find_lr_for_virq(vcpu, virq);
			if (lr_idx >= 0) {
				/*
				 * If the IRQ already has an LR slot, refresh it from descriptor
				 * state. Level timer state stays pending while CNTV is asserted;
				 * the dedicated timer sync paths clear it after the source lowers.
				 */
				ctx->lr[lr_idx] = make_vtimer_lr(vcpu, desc);
				if (!desc->level || !vgic_is_vtimer_irq(vcpu, virq)) {
					vgic_set_pending(vgic, vcpu->vcpu_id, desc, false);
				}
				continue;
			}

			if (ctx->used_lrs >= vgic->lr_count) {
				ctx->hcr |= ICH_HCR_UIE;
				break;
			}

			ctx->lr[ctx->used_lrs] = make_vtimer_lr(vcpu, desc);
			ctx->used_lrs++;
			if (!desc->level || !vgic_is_vtimer_irq(vcpu, virq)) {
				vgic_set_pending(vgic, vcpu->vcpu_id, desc, false);
			}
		}

		if ((ctx->hcr & ICH_HCR_UIE) != 0UL) {
			break;
		}
	}

	for (word = 0U; vgic->its_enabled && (word < ARM64_VGIC_LPI_WORDS); word++) {
		uint32_t bits = vgic->lpi_pending_bitmap[vcpu->vcpu_id][word];

		while (bits != 0U) {
			uint32_t bit = (uint32_t)ffs64((uint64_t)bits);
			uint32_t lpi = ARM64_VGIC_LPI_BASE + (word * 32U) + bit;
			struct arm64_vgic_irq *desc = &vgic->lpi[vcpu->vcpu_id][lpi - ARM64_VGIC_LPI_BASE];
			int32_t lr_idx;

			bits &= ~BIT32(bit);
			if (!desc->pending) {
				vgic_set_pending(vgic, vcpu->vcpu_id, desc, false);
				continue;
			}
			if (!vgic_irq_deliverable(vgic, vcpu, desc)) {
				continue;
			}

			lr_idx = find_lr_for_virq(vcpu, lpi);
			if (lr_idx >= 0) {
				ctx->lr[lr_idx] = make_lr(desc);
				vgic_set_pending(vgic, vcpu->vcpu_id, desc, false);
				continue;
			}
			if (ctx->used_lrs >= vgic->lr_count) {
				ctx->hcr |= ICH_HCR_UIE;
				break;
			}

			ctx->lr[ctx->used_lrs] = make_lr(desc);
			ctx->used_lrs++;
			vgic_set_pending(vgic, vcpu->vcpu_id, desc, false);
		}

		if ((ctx->hcr & ICH_HCR_UIE) != 0UL) {
			break;
		}
	}

	vgicv3_write_loaded_lrs(vcpu, is_current);
}

void arm64_vgicv3_flush_vcpu(struct acrn_vcpu *vcpu)
{
	if ((vcpu != NULL) && (vcpu->vm != NULL) && vcpu->vm->arch_vm.vgic.initialized) {
		vgicv3_flush_vcpu(vcpu, false);
	}
}

void arm64_vgicv3_flush_current_vcpu(struct acrn_vcpu *vcpu)
{
	if ((vcpu != NULL) && (vcpu->vm != NULL) && vcpu->vm->arch_vm.vgic.initialized) {
		vgicv3_flush_vcpu(vcpu, true);
	}
}

static void vgicv3_flush_target_vcpu(struct acrn_vcpu *current_vcpu,
	struct acrn_vcpu *target_vcpu)
{
	if (target_vcpu != NULL) {
		vgicv3_flush_vcpu(target_vcpu, target_vcpu == current_vcpu);
	}
}

static void vgicv3_flush_vm(struct acrn_vcpu *current_vcpu)
{
	struct acrn_vm *vm = current_vcpu->vm;
	struct acrn_vcpu *target_vcpu;
	uint16_t idx;

	foreach_vcpu(idx, vm, target_vcpu) {
		vgicv3_flush_target_vcpu(current_vcpu, target_vcpu);
	}
}

static int32_t vgic_inject_locked(struct arm64_vgicv3 *vgic, struct acrn_vcpu *target_vcpu,
	uint32_t virq, bool level)
{
	struct arm64_vgic_irq *desc = vgic_irq_desc(target_vcpu, virq);
	int32_t ret = -EINVAL;

	if (desc != NULL) {
		/*
		 * Always sync before injection so a guest-completed LR does not
		 * get overwritten by stale software state. The event request wakes
		 * a blocked vCPU and makes the scheduler re-enter pre-guest
		 * request processing.
		 *
		 *   event source -> descriptor.pending -> optional live flush
		 *        -> vCPU event request -> scheduler/guest return path
		 */
		vgicv3_sync_vcpu(target_vcpu, false);
		desc->level = level;
		vgic_set_pending(vgic, target_vcpu->vcpu_id, desc, true);
		if (vgicv3_vcpu_is_loaded(target_vcpu)) {
			vgicv3_flush_vcpu(target_vcpu, false);
		}
		ret = 0;
	}

	return ret;
}

static int32_t arm64_vgicv3_inject_irq_to(struct acrn_vcpu *source_vcpu,
	struct acrn_vcpu *target_vcpu, uint32_t virq, bool level)
{
	struct arm64_vgicv3 *vgic;
	uint64_t flags;
	int32_t ret = -EINVAL;

	if ((target_vcpu != NULL) && (target_vcpu->vm != NULL) && (virq < ARM64_VGIC_IRQ_NUM)) {
		vgic = &target_vcpu->vm->arch_vm.vgic;
		if (vgic->initialized) {
			spinlock_irqsave_obtain(&vgic->lock, &flags);
			ret = vgic_inject_locked(vgic, target_vcpu, virq, level);
			spinlock_irqrestore_release(&vgic->lock, flags);
			vgic_record_irq_injection(source_vcpu, target_vcpu, virq, level, ret);
			if (ret == 0) {
				vcpu_make_request(target_vcpu, ARM64_VCPU_REQUEST_EVENT);
				signal_event(&target_vcpu->events[ARM64_VCPU_EVENT_VIRTUAL_INTERRUPT]);
			}
		}
	}

	return ret;
}

int32_t arm64_vgicv3_inject_irq(struct acrn_vcpu *vcpu, uint32_t virq, bool level)
{
	struct arm64_vgicv3 *vgic;
	struct acrn_vcpu *target_vcpu;
	uint64_t flags;
	int32_t ret = -EINVAL;

	if ((vcpu != NULL) && (vcpu->vm != NULL) && (virq < ARM64_VGIC_IRQ_NUM)) {
		vgic = &vcpu->vm->arch_vm.vgic;
		if (vgic->initialized) {
			spinlock_irqsave_obtain(&vgic->lock, &flags);
			target_vcpu = vgic_irq_target_vcpu(vcpu, vgic, virq);
			ret = vgic_inject_locked(vgic, target_vcpu, virq, level);
			spinlock_irqrestore_release(&vgic->lock, flags);
			vgic_record_irq_injection(vcpu, target_vcpu, virq, level, ret);
			if (ret == 0) {
				vcpu_make_request(target_vcpu, ARM64_VCPU_REQUEST_EVENT);
				signal_event(&target_vcpu->events[ARM64_VCPU_EVENT_VIRTUAL_INTERRUPT]);
			}
		}
	}

	return ret;
}

int32_t arm64_vgicv3_inject_msi(struct acrn_vm *vm, uint32_t device_id, uint32_t event_id)
{
	struct arm64_vgicv3 *vgic;
	struct arm64_vits_event *event;
	uint64_t flags;
	int32_t ret = -EINVAL;

	if ((vm != NULL) && vm->arch_vm.vgic.initialized && vm->arch_vm.vgic.its_enabled) {
		vgic = &vm->arch_vm.vgic;
		spinlock_irqsave_obtain(&vgic->lock, &flags);
		event = vits_find_event(&vgic->its, device_id, event_id);
		if (event != NULL) {
			vits_inject_event_locked(vm, vgic, event);
			ret = 0;
		}
		spinlock_irqrestore_release(&vgic->lock, flags);
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
		vgicv3_sync_vcpu(vcpu, false);
		lr_idx = find_lr_for_virq(vcpu, virq);
		if (lr_idx >= 0) {
			remove_lr(vcpu, (uint32_t)lr_idx);
		}
		vgic_set_pending(vgic, vcpu->vcpu_id, desc, false);
		desc->active = false;
		if (vgicv3_vcpu_is_loaded(vcpu)) {
			vgicv3_flush_vcpu(vcpu, false);
		}
		spinlock_irqrestore_release(&vgic->lock, flags);
		vcpu_make_request(vcpu, ARM64_VCPU_REQUEST_EVENT);
		ret = 0;
	}

	return ret;
}

int32_t arm64_vgicv3_deassert_irq(struct acrn_vcpu *vcpu, uint32_t virq)
{
	struct arm64_vgic_irq *desc = vgic_irq_desc(vcpu, virq);
	struct arm64_vgicv3 *vgic;
	uint64_t flags;
	int32_t ret = -EINVAL;

	if (desc != NULL) {
		vgic = &vcpu->vm->arch_vm.vgic;
		spinlock_irqsave_obtain(&vgic->lock, &flags);
		vgicv3_sync_vcpu(vcpu, false);
		/*
		 * Device level deassertion lowers the input line; it is not a guest
		 * deactivate command. Preserve Active LRs until the guest EOI path
		 * completes them, but remove any pending redelivery for the line.
		 */
		vgic_set_pending(vgic, vcpu->vcpu_id, desc, false);
		vgic_update_irq_lr(vcpu, desc);
		if (vgicv3_vcpu_is_loaded(vcpu)) {
			vgicv3_flush_vcpu(vcpu, false);
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

static void vgic_record_sgi(struct acrn_vcpu *source_vcpu, uint64_t value,
	uint32_t intid, uint16_t target_mask, uint16_t delivered_mask, int32_t status)
{
	struct arm64_vcpu_last_sgi *last = &source_vcpu->arch.debug.last_sgi;

	last->value = value;
	last->intid = intid;
	last->status = status;
	last->source_vcpu_id = source_vcpu->vcpu_id;
	last->target_mask = target_mask;
	last->delivered_mask = delivered_mask;
	last->tsc = cpu_ticks();
}

static void vgic_sgi_masks(const struct acrn_vcpu *vcpu, uint16_t *enabled,
	uint16_t *pending, uint16_t *active)
{
	const struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
	uint32_t intid;

	*enabled = 0U;
	*pending = 0U;
	*active = 0U;

	for (intid = 0U; intid < ARM64_VGIC_SGI_NUM; intid++) {
		const struct arm64_vgic_irq *desc = &vgic->irq[vcpu->vcpu_id][intid];
		uint16_t bit = (uint16_t)BIT32(intid);

		if (desc->enabled) {
			*enabled |= bit;
		}
		if (desc->pending) {
			*pending |= bit;
		}
		if (desc->active) {
			*active |= bit;
		}
	}
}

static void vgic_record_sgi_target(struct acrn_vcpu *source_vcpu,
	struct acrn_vcpu *target_vcpu, uint64_t value, uint32_t intid, int32_t status)
{
	struct arm64_vcpu_last_sgi_target *last = &target_vcpu->arch.debug.last_sgi_target;
	const struct arm64_vgic_irq *desc = vgic_irq_desc(target_vcpu, intid);
	bool target_current = vgicv3_vcpu_is_loaded(target_vcpu);
	uint16_t enabled;
	uint16_t pending;
	uint16_t active;

	/*
	 * Remote target state can only be sampled from the saved vGIC context.
	 * Live ICH_* registers are read only when the target vCPU is the current
	 * guest on this pCPU; otherwise they would describe the source vCPU.
	 */
	vgic_sgi_masks(target_vcpu, &enabled, &pending, &active);
	last->source_value = value;
	last->intid = intid;
	last->status = status;
	last->source_vcpu_id = source_vcpu->vcpu_id;
	last->target_vcpu_id = target_vcpu->vcpu_id;
	last->local_enabled = enabled;
	last->local_pending = pending;
	last->local_active = active;
	last->used_lrs = target_vcpu->arch.vgic.used_lrs;
	last->request_pending = vcpu_has_pending_request(target_vcpu);
	last->target_running = (target_vcpu->state == VCPU_RUNNING);
	last->target_current = target_current;
	last->desc_enabled = (desc != NULL) && desc->enabled;
	last->desc_pending = (desc != NULL) && desc->pending;
	last->desc_active = (desc != NULL) && desc->active;
	last->desc_level = (desc != NULL) && desc->level;
	if (target_current) {
		last->hcr = read_ich_hcr_el2();
		last->misr = read_ich_misr_el2();
		last->lr0 = read_ich_lr_el2(0U);
		last->lr1 = read_ich_lr_el2(1U);
	} else {
		last->hcr = target_vcpu->arch.vgic.hcr;
		last->misr = 0UL;
		last->lr0 = (target_vcpu->arch.vgic.used_lrs > 0U) ?
			target_vcpu->arch.vgic.lr[0U] : 0UL;
		last->lr1 = (target_vcpu->arch.vgic.used_lrs > 1U) ?
			target_vcpu->arch.vgic.lr[1U] : 0UL;
	}
	last->tsc = cpu_ticks();
}

int32_t arm64_vgicv3_handle_sgi1r(struct acrn_vcpu *vcpu, uint64_t value)
{
	uint32_t intid = (uint32_t)((value >> ICC_SGI1R_INTID_SHIFT) & ICC_SGI1R_INTID_MASK);
	uint16_t idx;
	uint16_t target_mask = 0U;
	uint16_t delivered_mask = 0U;
	struct acrn_vcpu *target_vcpu;
	int32_t last_status = 0;

	/*
	 * Guest SGI sends are trapped from ICC_SGI1R_EL1 and translated into
	 * per-target virtual IRQ injections. The current affinity model matches
	 * the QEMU vMPIDR layout where vCPU IDs occupy affinity level 0.
	 *
	 *   ICC_SGI1R_EL1 trap -> target mask -> per-target inject
	 *        -> kick running target vCPU
	 */
	foreach_vcpu(idx, vcpu->vm, target_vcpu) {
		if (sgi1r_targets_vcpu(value, vcpu->vcpu_id, target_vcpu->vcpu_id)) {
			target_mask |= BIT32(target_vcpu->vcpu_id);
		}
		if ((target_vcpu->state != VCPU_OFFLINE) &&
			((target_mask & BIT32(target_vcpu->vcpu_id)) != 0U)) {
			last_status = arm64_vgicv3_inject_irq_to(vcpu, target_vcpu, intid, false);
			vgic_record_sgi_target(vcpu, target_vcpu, value, intid, last_status);
			if (last_status == 0) {
				delivered_mask |= BIT32(target_vcpu->vcpu_id);
			}
			if (target_vcpu->state == VCPU_RUNNING) {
				kick_vcpu(target_vcpu);
			}
		}
	}
	vgic_record_sgi(vcpu, value, intid, target_mask, delivered_mask, last_status);

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
		 *
		 *   maintenance PPI -> CPU interface snapshot
		 *        -> sync LRs -> flush pending backlog
		 */
		spinlock_irqsave_obtain(&vgic->lock, &flags);
		vgicv3_record_cpuif_snapshot(vcpu, &vcpu->arch.debug.last_vgic_maintenance,
			ARM64_VCPU_DEBUG_VGIC_MAINTENANCE);
		vgicv3_sync_vcpu(vcpu, true);
		vgicv3_flush_vcpu(vcpu, true);
		spinlock_irqrestore_release(&vgic->lock, flags);
	}
}

void arm64_vgicv3_update_current_vtimer(struct acrn_vcpu *vcpu)
{
	struct arm64_vgicv3 *vgic;
	uint64_t flags;
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
	level = vgic_sample_current_vtimer(vcpu);

	vgic = &vcpu->vm->arch_vm.vgic;
	spinlock_irqsave_obtain(&vgic->lock, &flags);
	/*
	 * CNTV can expire while the guest has already acknowledged a timer LR.
	 * Sync before changing the timer pending bit so the software descriptor
	 * sees the hardware Active state and flush preserves Active+Pending
	 * instead of overwriting it with a stale Pending-only LR.
	 *
	 *   sysreg write or return path -> sample CNTV -> sync old LR
	 *        -> update timer descriptor -> flush deliverable timer state
	 */
	vgicv3_sync_vcpu(vcpu, true);
	vgicv3_sync_timer_line_locked(vcpu, true, level);
	if (level || vcpu->arch.gctx.cntv_el2_masked) {
		arm64_vcpu_trace_vtimer(vcpu, ARM64_VTIMER_TRACE_UPDATE, virq,
			UINT32_MAX, UINT64_MAX, false, false);
	}
	rescue = vgicv3_vtimer_live_stuck(vcpu);
	spinlock_irqrestore_release(&vgic->lock, flags);
	if (rescue) {
		vgicv3_arm_vtimer_stuck_rescue(vcpu);
	}
}

static int32_t vgicv3_inject_current_timer(struct acrn_vcpu *vcpu, uint32_t virq,
	uint32_t guest_ctl, uint64_t guest_cval)
{
	int32_t ret;

	vgicv3_disarm_vtimer_backup(vcpu);
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

void arm64_vgicv3_virtual_timer_irq_handler(__unused uint32_t irq, __unused void *data)
{
	struct acrn_vcpu *vcpu = get_running_vcpu(get_pcpu_id());

	/*
	 * CNTV is the hardware timer context loaded for the current vCPU. Its PPI
	 * is physically delivered to the pCPU running that vCPU, then translated
	 * into the guest-visible PPI recorded in timer_virq. That lets CNTP-based
	 * RTOS guests keep their expected interrupt number while the host still
	 * reserves CNTP for scheduler ticks.
	 *
	 *   physical PPI27 -> sample live CNTV -> mask host line
	 *        -> inject guest timer PPI through vGIC
	 */
	if (vcpu != NULL) {
		uint32_t virq = vcpu->arch.gctx.timer_virq;
		uint32_t guest_ctl;
		uint64_t guest_cval;

		/*
		 * Some CPU models allow EL1 CNTV access without trapping. When the
		 * physical virtual-timer PPI arrives, the hardware CNTV registers are
		 * therefore the authoritative guest timer state for this loaded vCPU.
		 */
		if (virq == 0U) {
			vcpu->arch.gctx.timer_virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
			virq = ARM64_GIC_PPI_VIRTUAL_TIMER;
		}
		if (virq == ARM64_GIC_PPI_PHYSICAL_TIMER) {
			vcpu->arch.gctx.cntp_cval_el0 = read_cntv_cval_el0();
			vcpu->arch.gctx.cntp_ctl_el0 = vgic_live_vtimer_ctl(vcpu);
		} else {
			vcpu->arch.gctx.cntv_cval_el0 = read_cntv_cval_el0();
			vcpu->arch.gctx.cntv_ctl_el0 = vgic_live_vtimer_ctl(vcpu);
		}

		/*
		 * The hardware virtual timer is level-triggered. Once the deadline
		 * expires, the PPI remains asserted until the guest programs a new
		 * deadline or masks/disables the timer. Mask the live CNTV output while
		 * the virtual IRQ is in flight, but keep the guest shadow state as the
		 * guest programmed it.
		 */
		vgic_timer_set_host_mask(vcpu, true);
		if (virq == ARM64_GIC_PPI_PHYSICAL_TIMER) {
			guest_ctl = vcpu->arch.gctx.cntp_ctl_el0;
			guest_cval = vcpu->arch.gctx.cntp_cval_el0;
		} else {
			guest_ctl = vcpu->arch.gctx.cntv_ctl_el0;
			guest_cval = vcpu->arch.gctx.cntv_cval_el0;
		}
		arm64_vcpu_trace_vtimer(vcpu, ARM64_VTIMER_TRACE_PPI, virq,
			guest_ctl, guest_cval, false, false);
		(void)vgicv3_inject_current_timer(vcpu, virq, guest_ctl, guest_cval);
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

	/*
	 * Normal delivery follows the physical virtual-timer PPI handler above.
	 * QEMU can leave the Linux BSP with CNTV enabled and an expired deadline
	 * while PPI27 is not asserted. On guest exits, check only the loaded vCPU
	 * and translate that expired CNTV state into the same guest PPI.
	 *
	 *   guest exit -> read live CNTV -> if expired and not masked
	 *        -> inject as if the physical virtual-timer PPI fired
	 */
	if (vcpu->arch.gctx.cntv_el2_masked) {
		(void)vgicv3_requeue_lost_masked_timer(vcpu);
		return;
	}
	ctl = vgic_live_vtimer_ctl(vcpu);
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
	vgic_timer_set_host_mask(vcpu, true);
	arm64_vcpu_trace_vtimer(vcpu, ARM64_VTIMER_TRACE_POLL,
		vcpu->arch.gctx.timer_virq, ctl, cval, false, false);
	if (vcpu->arch.gctx.timer_virq == ARM64_GIC_PPI_PHYSICAL_TIMER) {
		(void)vgicv3_inject_current_timer(vcpu, ARM64_GIC_PPI_PHYSICAL_TIMER,
			vcpu->arch.gctx.cntp_ctl_el0, cval);
	} else {
		(void)vgicv3_inject_current_timer(vcpu, ARM64_GIC_PPI_VIRTUAL_TIMER,
			vcpu->arch.gctx.cntv_ctl_el0, cval);
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
		mmio_access_in_range(offset, size, VGICD_IPRIORITYR, VGICD_ITARGETSR);
}

static bool vgic_target_access(uint32_t offset, uint64_t size)
{
	return ((size == 1UL) || (size == 2UL) || (size == 4UL)) &&
		mmio_access_in_range(offset, size, VGICD_ITARGETSR, VGICD_ICFGR);
}

static bool vgic_word_access(uint32_t offset, uint64_t size)
{
	return (size == 4UL) && ((offset & 0x3U) == 0U);
}

struct vgic_irq_bit_reg {
	uint32_t selector;
	uint32_t irq_base;
};

struct vgic_irq_config_reg {
	uint32_t irq_base;
};

struct vgic_irouter_reg {
	uint32_t virq;
	uint32_t word;
};

struct vgicr_frame {
	uint16_t pcpu_id;
	uint16_t vcpu_id;
	uint32_t offset;
};

static bool vgic_decode_irq_bit_reg(uint32_t offset, bool local_bank,
	struct vgic_irq_bit_reg *reg)
{
	uint32_t selector = UINT32_MAX;
	uint32_t base = 0U;
	uint32_t max_words = local_bank ? 1U : ARM64_VGIC_WORDS;
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
	case VGICD_IGROUPR ... (VGICD_ISENABLER - 1U):
		selector = VGICD_IGROUPR;
		base = reg_word_index(offset, VGICD_IGROUPR) * 32U;
		break;
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
		if ((offset >= VGICD_IGRPMODR) &&
			(offset < (VGICD_IGRPMODR + (max_words * 4U)))) {
			selector = VGICD_IGRPMODR;
			base = reg_word_index(offset, VGICD_IGRPMODR) * 32U;
		} else {
			ret = false;
		}
		break;
	}

	if (ret && local_bank && (base != 0U)) {
		ret = false;
	}

	if (ret) {
		reg->selector = selector;
		reg->irq_base = local_bank ? 0U : base;
	}

	return ret;
}

static bool vgic_decode_irq_config_reg(uint32_t offset, bool local_bank,
	struct vgic_irq_config_reg *reg)
{
	uint32_t max_irqs = local_bank ? ARM64_VGIC_LOCAL_IRQ_NUM : ARM64_VGIC_IRQ_NUM;
	uint32_t end = VGICD_ICFGR + ((max_irqs / VGIC_ICFGR_IRQS_PER_REG) * 4U);
	uint32_t word;
	uint32_t irq_base;
	bool ret = false;

	if ((offset >= VGICD_ICFGR) && (offset < end)) {
		word = reg_word_index(offset, VGICD_ICFGR);
		irq_base = word * VGIC_ICFGR_IRQS_PER_REG;
		reg->irq_base = irq_base;
		ret = true;
	}

	return ret;
}

static bool vgic_decode_irouter_reg(uint32_t offset, struct vgic_irouter_reg *reg)
{
	uint32_t rel;
	uint32_t virq;
	bool ret = false;

	if (offset >= VGICD_IROUTER) {
		rel = offset - VGICD_IROUTER;
		virq = rel / 8U;
		if ((virq >= ARM64_VGIC_LOCAL_IRQ_NUM) && (virq < ARM64_VGIC_IRQ_NUM) &&
			((rel & 0x3U) == 0U)) {
			reg->virq = virq;
			reg->word = ((rel & 0x4U) != 0U) ? 1U : 0U;
			ret = true;
		}
	}

	return ret;
}

static uint64_t vgic_read_irq_priority(struct acrn_vcpu *vcpu, uint32_t irq_base, uint64_t size)
{
	struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
	uint64_t value = 0UL;
	uint32_t byte;

	for (byte = 0U; byte < size; byte++) {
		uint32_t virq = irq_base + byte;
		struct arm64_vgic_irq *desc = vgic_irq_is_spi(virq) ?
			&vgic->irq[0U][virq] : vgic_irq_desc(vcpu, virq);
		uint8_t priority = (desc != NULL) ? desc->priority : GIC_PRIORITY_DEFAULT;

		value |= (uint64_t)priority << (byte * 8U);
	}

	return value;
}

static void vgic_write_irq_priority(struct acrn_vcpu *vcpu, uint32_t irq_base,
	uint64_t size, uint64_t value)
{
	struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
	uint32_t byte;

	for (byte = 0U; byte < size; byte++) {
		uint32_t virq = irq_base + byte;
		uint8_t priority = (uint8_t)((value >> (byte * 8U)) & 0xffU);
		uint16_t vcpu_id;
		struct arm64_vgic_irq *desc;

		if (vgic_irq_is_spi(virq)) {
			for (vcpu_id = 0U; vcpu_id < vgic->vcpu_count; vcpu_id++) {
				desc = &vgic->irq[vcpu_id][virq];
				desc->priority = priority;
				vgic_update_irq_row_lr(vcpu->vm, vcpu_id, desc);
			}
		} else {
			desc = vgic_irq_desc(vcpu, virq);
			if (desc == NULL) {
				continue;
			}
			desc->priority = priority;
			vgic_update_irq_lr(vcpu, desc);
		}
	}
}

static struct arm64_vgic_irq *vgic_read_irq_bit_desc(struct acrn_vcpu *vcpu,
	uint32_t virq, uint32_t selector)
{
	struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
	uint16_t target_vcpu_id = vcpu->vcpu_id;

	if (virq >= ARM64_VGIC_IRQ_NUM) {
		return NULL;
	}

	if (vgic_irq_is_spi(virq)) {
		switch (selector) {
		case VGICD_ISPENDR:
		case VGICD_ICPENDR:
		case VGICD_ISACTIVER:
		case VGICD_ICACTIVER:
			target_vcpu_id = vgic_valid_target_vcpu(vgic, vgic->irq[0U][virq].target_vcpu);
			break;
		default:
			target_vcpu_id = 0U;
			break;
		}
	}

	return (target_vcpu_id < ARM64_VGIC_MAX_VCPUS) ? &vgic->irq[target_vcpu_id][virq] : NULL;
}

static uint32_t vgic_read_irq_bits(struct acrn_vcpu *vcpu, uint32_t irq_base, uint32_t selector)
{
	uint32_t val = 0U;
	uint32_t bit;

	for (bit = 0U; bit < 32U; bit++) {
		struct arm64_vgic_irq *desc = vgic_read_irq_bit_desc(vcpu,
			irq_base + bit, selector);
		bool set = false;

		if (desc == NULL) {
			continue;
		}

		switch (selector) {
		case VGICD_IGROUPR:
			set = true;
			break;
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
		case VGICD_IGRPMODR:
			set = false;
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
	struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
	uint32_t bit;

	for (bit = 0U; bit < 32U; bit++) {
		struct arm64_vgic_irq *desc;
		uint32_t virq = irq_base + bit;
		uint16_t vcpu_id;
		uint16_t target_vcpu_id;

		if (virq >= ARM64_VGIC_IRQ_NUM) {
			continue;
		}

		if ((selector != VGICD_IGROUPR) && (selector != VGICD_IGRPMODR) &&
			((val & BIT32(bit)) == 0U)) {
			continue;
		}

		switch (selector) {
		case VGICD_IGROUPR:
			/* The current virtual GIC exposes a non-secure Group-1-only model. */
			break;
		case VGICD_ISENABLER:
			if (vgic_irq_is_spi(virq)) {
				for (vcpu_id = 0U; vcpu_id < vgic->vcpu_count; vcpu_id++) {
					desc = &vgic->irq[vcpu_id][virq];
					desc->enabled = true;
					vgic_update_irq_row_lr(vcpu->vm, vcpu_id, desc);
				}
			} else {
				desc = vgic_irq_desc(vcpu, virq);
				if (desc != NULL) {
					desc->enabled = true;
					vgic_update_irq_lr(vcpu, desc);
				}
			}
			break;
		case VGICD_ICENABLER:
			if (vgic_irq_is_spi(virq)) {
				for (vcpu_id = 0U; vcpu_id < vgic->vcpu_count; vcpu_id++) {
					desc = &vgic->irq[vcpu_id][virq];
					desc->enabled = false;
					vgic_update_irq_row_lr(vcpu->vm, vcpu_id, desc);
				}
			} else {
				desc = vgic_irq_desc(vcpu, virq);
				if (desc != NULL) {
					desc->enabled = false;
					vgic_update_irq_lr(vcpu, desc);
				}
			}
			break;
		case VGICD_ISPENDR:
			target_vcpu_id = vgic_irq_is_spi(virq) ?
				vgic_valid_target_vcpu(vgic, vgic->irq[0U][virq].target_vcpu) :
				vcpu->vcpu_id;
			desc = &vgic->irq[target_vcpu_id][virq];
			vgic_set_pending(vgic, target_vcpu_id, desc, true);
			vgic_update_irq_row_lr(vcpu->vm, target_vcpu_id, desc);
			break;
		case VGICD_ICPENDR:
			target_vcpu_id = vgic_irq_is_spi(virq) ?
				vgic_valid_target_vcpu(vgic, vgic->irq[0U][virq].target_vcpu) :
				vcpu->vcpu_id;
			desc = &vgic->irq[target_vcpu_id][virq];
			vgic_set_pending(vgic, target_vcpu_id, desc, false);
			vgic_update_irq_row_lr(vcpu->vm, target_vcpu_id, desc);
			break;
		case VGICD_ISACTIVER:
			target_vcpu_id = vgic_irq_is_spi(virq) ?
				vgic_valid_target_vcpu(vgic, vgic->irq[0U][virq].target_vcpu) :
				vcpu->vcpu_id;
			desc = &vgic->irq[target_vcpu_id][virq];
			desc->active = true;
			vgic_update_irq_row_lr(vcpu->vm, target_vcpu_id, desc);
			break;
		case VGICD_ICACTIVER:
			target_vcpu_id = vgic_irq_is_spi(virq) ?
				vgic_valid_target_vcpu(vgic, vgic->irq[0U][virq].target_vcpu) :
				vcpu->vcpu_id;
			desc = &vgic->irq[target_vcpu_id][virq];
			desc->active = false;
			vgic_update_irq_row_lr(vcpu->vm, target_vcpu_id, desc);
			break;
		case VGICD_IGRPMODR:
			/* Group modifiers are ignored in the Group-1-only model. */
			break;
		default:
			break;
		}
	}
}

static uint64_t vgic_read_irq_target(struct acrn_vcpu *vcpu, uint32_t irq_base, uint64_t size)
{
	struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
	uint64_t value = 0UL;
	uint32_t byte;

	for (byte = 0U; byte < size; byte++) {
		uint32_t virq = irq_base + byte;
		uint8_t target = 0U;

		if (vgic_irq_is_spi(virq)) {
			uint16_t target_vcpu_id =
				vgic_valid_target_vcpu(vgic, vgic->irq[0U][virq].target_vcpu);

			target = (uint8_t)(1U << target_vcpu_id);
		}
		value |= (uint64_t)target << (byte * 8U);
	}

	return value;
}

static void vgic_write_irq_target(struct acrn_vcpu *vcpu, uint32_t irq_base,
	uint64_t size, uint64_t value)
{
	uint32_t byte;

	for (byte = 0U; byte < size; byte++) {
		uint32_t virq = irq_base + byte;
		uint8_t target_mask = (uint8_t)((value >> (byte * 8U)) & 0xffU);
		uint16_t target_vcpu_id = 0U;

		if (!vgic_irq_is_spi(virq)) {
			continue;
		}

		if (target_mask != 0U) {
			target_vcpu_id = ffs64((uint64_t)target_mask);
		}
		vgic_set_spi_target(vcpu->vm, virq, target_vcpu_id);
	}
}

static uint32_t vgic_read_irq_config(struct acrn_vcpu *vcpu, uint32_t irq_base)
{
	struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
	uint32_t val = 0U;
	uint32_t idx;

	for (idx = 0U; idx < VGIC_ICFGR_IRQS_PER_REG; idx++) {
		uint32_t virq = irq_base + idx;
		struct arm64_vgic_irq *desc;

		if (virq >= ARM64_VGIC_IRQ_NUM) {
			continue;
		}

		desc = vgic_irq_is_spi(virq) ? &vgic->irq[0U][virq] : vgic_irq_desc(vcpu, virq);

		if ((desc != NULL) && !desc->level) {
			val |= BIT32((idx * 2U) + 1U);
		}
	}

	return val;
}

static void vgic_write_irq_config(struct acrn_vcpu *vcpu, uint32_t irq_base, uint32_t val)
{
	struct arm64_vgicv3 *vgic = &vcpu->vm->arch_vm.vgic;
	uint32_t idx;

	for (idx = 0U; idx < VGIC_ICFGR_IRQS_PER_REG; idx++) {
		uint32_t virq = irq_base + idx;
		uint16_t vcpu_id;
		struct arm64_vgic_irq *desc;

		if (virq >= ARM64_VGIC_IRQ_NUM) {
			continue;
		}

		if (virq < ARM64_VGIC_SGI_NUM) {
			continue;
		}

		if (vgic_irq_is_spi(virq)) {
			for (vcpu_id = 0U; vcpu_id < vgic->vcpu_count; vcpu_id++) {
				desc = &vgic->irq[vcpu_id][virq];
				desc->level = ((val & BIT32((idx * 2U) + 1U)) == 0U);
				vgic_update_irq_row_lr(vcpu->vm, vcpu_id, desc);
			}
		} else {
			desc = vgic_irq_desc(vcpu, virq);
			if (desc == NULL) {
				continue;
			}
			desc->level = ((val & BIT32((idx * 2U) + 1U)) == 0U);
			vgic_update_irq_lr(vcpu, desc);
		}
	}
}

static uint16_t vgic_irouter_target_vcpu(const struct arm64_vgicv3 *vgic, uint64_t router)
{
	uint64_t aff0 = router & GIC_IROUTER_AFF0_MASK;
	uint64_t aff1 = (router >> GIC_IROUTER_AFF1_SHIFT) & GIC_IROUTER_AFF_MASK;
	uint64_t aff2 = (router >> GIC_IROUTER_AFF2_SHIFT) & GIC_IROUTER_AFF_MASK;
	uint64_t aff3 = (router >> GIC_IROUTER_AFF3_SHIFT) & GIC_IROUTER_AFF_MASK;
	uint16_t target_vcpu_id = 0U;

	if (((router & GIC_IROUTER_IRM) == 0UL) && (aff1 == 0UL) && (aff2 == 0UL) &&
		(aff3 == 0UL) && (aff0 < vgic->vcpu_count)) {
		target_vcpu_id = (uint16_t)aff0;
	}

	return target_vcpu_id;
}

static uint32_t vgic_read_irouter_word(const struct arm64_vgicv3 *vgic,
	const struct vgic_irouter_reg *reg)
{
	uint32_t spi = reg->virq - ARM64_VGIC_LOCAL_IRQ_NUM;
	uint64_t router = (spi < ARM64_VGIC_SPI_NUM) ? vgic->spi_router[spi] : 0UL;
	uint32_t value;

	switch (reg->word) {
	case 0U:
		value = (uint32_t)(router & GIC_IROUTER_LOW_MASK);
		break;
	case 1U:
		value = (uint32_t)(router >> 32U);
		break;
	default:
		value = 0U;
		break;
	}

	return value;
}

static void vgic_write_irouter_word(struct acrn_vm *vm, struct arm64_vgicv3 *vgic,
	const struct vgic_irouter_reg *reg, uint32_t value)
{
	uint32_t spi = reg->virq - ARM64_VGIC_LOCAL_IRQ_NUM;
	uint64_t router = (spi < ARM64_VGIC_SPI_NUM) ? vgic->spi_router[spi] : 0UL;
	uint16_t target_vcpu_id;

	switch (reg->word) {
	case 0U:
		router = (router & ~GIC_IROUTER_LOW_MASK) | value;
		break;
	case 1U:
		router = (router & GIC_IROUTER_LOW_MASK) | ((uint64_t)value << 32U);
		break;
	default:
		break;
	}

	if (spi < ARM64_VGIC_SPI_NUM) {
		vgic->spi_router[spi] = router;
	}

	target_vcpu_id = vgic_irouter_target_vcpu(vgic, router);
	vgic_set_spi_target(vm, reg->virq, target_vcpu_id);
}

static struct arm64_vits_collection *vits_find_collection(struct arm64_vits *its,
	uint16_t collection_id)
{
	uint32_t idx;

	for (idx = 0U; idx < ARM64_VGIC_ITS_COLLECTION_NUM; idx++) {
		if (its->collection[idx].collection_id == collection_id) {
			return &its->collection[idx];
		}
	}

	return NULL;
}

static struct arm64_vits_device *vits_find_device(struct arm64_vits *its, uint32_t device_id)
{
	uint32_t idx;

	for (idx = 0U; idx < ARM64_VGIC_ITS_DEVICE_NUM; idx++) {
		if (its->device[idx].valid && (its->device[idx].device_id == device_id)) {
			return &its->device[idx];
		}
	}

	return NULL;
}

static struct arm64_vits_device *vits_alloc_device(struct arm64_vits *its, uint32_t device_id)
{
	uint32_t idx;

	for (idx = 0U; idx < ARM64_VGIC_ITS_DEVICE_NUM; idx++) {
		if (!its->device[idx].valid) {
			its->device[idx].device_id = device_id;
			its->device[idx].valid = true;
			return &its->device[idx];
		}
	}

	return NULL;
}

static uint32_t vits_device_index(const struct arm64_vits *its,
	const struct arm64_vits_device *device)
{
	uint32_t idx = ARM64_VGIC_ITS_DEVICE_NUM;

	if (device != NULL) {
		idx = (uint32_t)(device - its->device);
		if (idx >= ARM64_VGIC_ITS_DEVICE_NUM) {
			idx = ARM64_VGIC_ITS_DEVICE_NUM;
		}
	}

	return idx;
}

static struct arm64_vits_event *vits_find_event(struct arm64_vits *its,
	uint32_t device_id, uint32_t event_id)
{
	uint32_t dev_idx;
	uint32_t evt_idx;

	for (dev_idx = 0U; dev_idx < ARM64_VGIC_ITS_DEVICE_NUM; dev_idx++) {
		if (!its->device[dev_idx].valid || (its->device[dev_idx].device_id != device_id)) {
			continue;
		}
		for (evt_idx = 0U; evt_idx < ARM64_VGIC_ITS_EVENT_NUM; evt_idx++) {
			if (its->event[dev_idx][evt_idx].valid &&
				(its->event[dev_idx][evt_idx].event_id == event_id)) {
				return &its->event[dev_idx][evt_idx];
			}
		}
	}

	return NULL;
}

static struct arm64_vits_event *vits_alloc_event(struct arm64_vits *its,
	uint32_t device_id, uint32_t event_id)
{
	uint32_t dev_idx;
	uint32_t evt_idx;

	for (dev_idx = 0U; dev_idx < ARM64_VGIC_ITS_DEVICE_NUM; dev_idx++) {
		if (!its->device[dev_idx].valid || (its->device[dev_idx].device_id != device_id)) {
			continue;
		}
		if (event_id >= its->device[dev_idx].event_count) {
			break;
		}
		for (evt_idx = 0U; evt_idx < ARM64_VGIC_ITS_EVENT_NUM; evt_idx++) {
			if (!its->event[dev_idx][evt_idx].valid) {
				its->event[dev_idx][evt_idx].device_id = device_id;
				its->event[dev_idx][evt_idx].event_id = event_id;
				its->event[dev_idx][evt_idx].valid = true;
				return &its->event[dev_idx][evt_idx];
			}
		}
	}

	return NULL;
}

static uint16_t vits_target_vcpu_id(struct arm64_vgicv3 *vgic, uint16_t collection_id)
{
	struct arm64_vits_collection *collection = vits_find_collection(&vgic->its, collection_id);
	uint16_t target = VGIC_INVALID_VCPU_ID;

	if ((collection != NULL) && collection->valid) {
		target = vgic_valid_target_vcpu(vgic, collection->target_vcpu);
	}

	return target;
}

static void vits_inject_event_locked(struct acrn_vm *vm, struct arm64_vgicv3 *vgic,
	struct arm64_vits_event *event)
{
	struct acrn_vcpu *target_vcpu;
	struct arm64_vgic_irq *desc;
	uint16_t target_vcpu_id;

	if ((event == NULL) || !event->valid || !vgic_irq_is_lpi(event->lpi)) {
		return;
	}

	target_vcpu_id = vits_target_vcpu_id(vgic, event->collection_id);
	if (target_vcpu_id >= vgic->vcpu_count) {
		return;
	}
	target_vcpu = vcpu_from_vid(vm, target_vcpu_id);
	desc = vgic_irq_desc(target_vcpu, event->lpi);
	if (desc != NULL) {
		desc->target_vcpu = (uint8_t)target_vcpu_id;
		desc->level = false;
		vgic_inject_locked(vgic, target_vcpu, event->lpi, false);
		if (target_vcpu->state != VCPU_OFFLINE) {
			vcpu_make_request(target_vcpu, ARM64_VCPU_REQUEST_EVENT);
			signal_event(&target_vcpu->events[ARM64_VCPU_EVENT_VIRTUAL_INTERRUPT]);
			if (target_vcpu->state == VCPU_RUNNING) {
				kick_vcpu(target_vcpu);
			}
		}
	}
}

static void vits_drop_device(struct arm64_vits *its, uint32_t device_id)
{
	uint32_t dev_idx;
	uint32_t evt_idx;

	for (dev_idx = 0U; dev_idx < ARM64_VGIC_ITS_DEVICE_NUM; dev_idx++) {
		if (!its->device[dev_idx].valid || (its->device[dev_idx].device_id != device_id)) {
			continue;
		}
		for (evt_idx = 0U; evt_idx < ARM64_VGIC_ITS_EVENT_NUM; evt_idx++) {
			its->event[dev_idx][evt_idx].valid = false;
		}
		(void)memset(&its->device[dev_idx], 0U, sizeof(its->device[dev_idx]));
		break;
	}
}

static void vits_execute_cmd(struct acrn_vm *vm, struct arm64_vgicv3 *vgic,
	const uint64_t cmd[4])
{
	struct arm64_vits *its = &vgic->its;
	uint32_t opcode = (uint32_t)(cmd[0] & 0xffUL);
	uint32_t device_id = (uint32_t)(cmd[0] >> 32U);
	uint32_t event_id = (uint32_t)(cmd[1] & UINT32_MAX);
	uint32_t lpi = (uint32_t)(cmd[1] >> 32U);
	uint16_t collection_id = (uint16_t)(cmd[2] & 0xffffUL);
	bool valid = ((cmd[2] & (1UL << 63U)) != 0UL);
	struct arm64_vits_device *device;
	struct arm64_vits_event *event;
	struct arm64_vits_collection *collection;
	uint32_t dev_idx;

	switch (opcode) {
	case GITS_CMD_MAPD:
		if (!valid) {
			vits_drop_device(its, device_id);
			break;
		}
		device = vits_find_device(its, device_id);
		if (device == NULL) {
			device = vits_alloc_device(its, device_id);
		}
		if (device != NULL) {
			uint32_t size = (uint32_t)(cmd[1] & 0x1fUL) + 1U;
			uint64_t itt_addr = cmd[2] & GITS_CMD_ITT_ADDRESS_MASK;
			uint16_t event_count = (size < 16U) ? (uint16_t)(1U << size) :
				ARM64_VGIC_ITS_EVENT_NUM;

			if (event_count > ARM64_VGIC_ITS_EVENT_NUM) {
				event_count = ARM64_VGIC_ITS_EVENT_NUM;
			}
			if ((device->itt_addr != itt_addr) || (device->event_count != event_count)) {
				dev_idx = vits_device_index(its, device);
				if (dev_idx < ARM64_VGIC_ITS_DEVICE_NUM) {
					(void)memset(its->event[dev_idx], 0U, sizeof(its->event[dev_idx]));
				}
			}

			device->itt_addr = itt_addr;
			device->event_count = event_count;
		}
		break;
	case GITS_CMD_MAPC:
		collection = vits_find_collection(its, collection_id);
		if (collection != NULL) {
			collection->valid = valid;
			collection->target_vcpu =
				vgic_valid_target_vcpu(vgic, (uint16_t)((cmd[2] >> 16U) & 0xffffU));
		}
		break;
	case GITS_CMD_MAPI:
		lpi = ARM64_VGIC_LPI_BASE + event_id;
		/* fall through */
	case GITS_CMD_MAPTI:
		device = vits_find_device(its, device_id);
		collection = vits_find_collection(its, collection_id);
		if (!vgic_irq_is_lpi(lpi) || (device == NULL) ||
			(event_id >= device->event_count) ||
			(collection == NULL) || !collection->valid) {
			break;
		}
		event = vits_find_event(its, device_id, event_id);
		if (event == NULL) {
			event = vits_alloc_event(its, device_id, event_id);
		}
		if (event != NULL) {
			event->lpi = lpi;
			event->collection_id = collection_id;
			if (vgic_irq_is_lpi(lpi)) {
				uint32_t lpi_idx = vgic_lpi_index(lpi);
				uint16_t target;

				for (target = 0U; target < vgic->vcpu_count; target++) {
					vgic->lpi[target][lpi_idx].enabled = true;
					vgic->lpi[target][lpi_idx].priority = GIC_PRIORITY_DEFAULT;
				}
			}
		}
		break;
	case GITS_CMD_MOVI:
		event = vits_find_event(its, device_id, event_id);
		collection = vits_find_collection(its, collection_id);
		if ((event != NULL) && (collection != NULL) && collection->valid) {
			event->collection_id = collection_id;
		}
		break;
	case GITS_CMD_DISCARD:
		event = vits_find_event(its, device_id, event_id);
		if (event != NULL) {
			event->valid = false;
		}
		break;
	case GITS_CMD_INT:
		vits_inject_event_locked(vm, vgic, vits_find_event(its, device_id, event_id));
		break;
	case GITS_CMD_CLEAR:
		event = vits_find_event(its, device_id, event_id);
		if ((event != NULL) && vgic_irq_is_lpi(event->lpi)) {
			uint16_t target = vits_target_vcpu_id(vgic, event->collection_id);

			if (target < vgic->vcpu_count) {
				struct arm64_vgic_irq *desc =
					&vgic->lpi[target][vgic_lpi_index(event->lpi)];

				vgic_set_pending(vgic, target, desc, false);
				desc->active = false;
				vgic_update_irq_row_lr(vm, target, desc);
			}
		}
		break;
	case GITS_CMD_INV:
	case GITS_CMD_INVALL:
	case GITS_CMD_SYNC:
	case GITS_CMD_MOVALL:
		break;
	default:
		break;
	}
}

static uint64_t vits_cmd_queue_base(const struct arm64_vits *its)
{
	return its->cbaser & GITS_CBASER_ADDRESS_MASK;
}

static uint64_t vits_cmd_queue_size(const struct arm64_vits *its)
{
	return (((its->cbaser & GITS_CBASER_SIZE_MASK) + 1UL) * PAGE_SIZE);
}

static bool vits_process_command_queue(struct acrn_vm *vm, struct arm64_vgicv3 *vgic)
{
	struct arm64_vits *its = &vgic->its;
	uint64_t base = vits_cmd_queue_base(its);
	uint64_t size = vits_cmd_queue_size(its);
	uint64_t reader = its->creadr;
	uint64_t writer = its->cwriter;
	uint32_t processed = 0U;
	bool flush = false;

	if (((its->ctlr & GITS_CTLR_ENABLE) == 0U) ||
		((its->cbaser & GITS_CBASER_VALID) == 0UL) ||
		(size < GITS_CMD_SIZE)) {
		its->creadr = writer;
		return false;
	}

	/*
	 * The ITS command queue is guest owned, but EL2 processes it while holding
	 * the vGIC lock. Enforce command-size alignment and a per-entry budget so
	 * malformed queues cannot keep unrelated vGIC operations blocked forever.
	 */
	if ((reader >= size) || (writer >= size) ||
		!vits_offset_aligned(reader) || !vits_offset_aligned(writer)) {
		its->creadr = writer;
		return false;
	}

	while ((reader != writer) && (processed < GITS_CMD_QUEUE_BUDGET)) {
		uint64_t cmd[4];

		if (copy_from_gpa(vm, cmd, base + reader, sizeof(cmd)) != 0) {
			break;
		}
		vits_execute_cmd(vm, vgic, cmd);
		flush = true;
		reader += GITS_CMD_SIZE;
		processed++;
		if (reader >= size) {
			reader = 0UL;
		}
	}

	its->creadr = reader;
	return flush;
}

static void vits_write_translater(struct acrn_vm *vm, struct arm64_vgicv3 *vgic,
	uint32_t event_id)
{
	/*
	 * A real MSI write carries the requester/device identity in the bus
	 * transaction. The current QEMU SDK path has no PCI requester plumbing yet,
	 * so direct writes to the vITS doorbell use DeviceID 0 as a deterministic
	 * local test path. Passthrough/MSI code should call arm64_vgicv3_inject_msi()
	 * with the real DeviceID once that layer exists.
	 */
	vits_inject_event_locked(vm, vgic, vits_find_event(&vgic->its, 0U, event_id));
}

static void vgicd_mmio_read(struct acrn_vcpu *vcpu, struct arm64_vgicv3 *vgic,
	struct acrn_mmio_request *mmio, uint32_t offset)
{
	struct vgic_irq_bit_reg bit_reg;
	struct vgic_irq_config_reg config_reg;
	struct vgic_irouter_reg irouter_reg;

	/*
	 * Distributor MMIO exposes the shared interrupt-controller programming
	 * model. For the current QEMU guests, the important pieces are enable,
	 * pending, active, priority, and basic identity registers.
	 */
	if (vgic_decode_irq_bit_reg(offset, false, &bit_reg)) {
		mmio->value = vgic_read_irq_bits(vcpu, bit_reg.irq_base, bit_reg.selector);
	} else if (vgic_decode_irq_config_reg(offset, false, &config_reg)) {
		mmio->value = vgic_read_irq_config(vcpu, config_reg.irq_base);
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
		case VGICD_ITARGETSR ... (VGICD_ICFGR - 1U):
			mmio->value = vgic_read_irq_target(vcpu, offset - VGICD_ITARGETSR,
				mmio->size);
			break;
		case VGICD_IROUTER ... (VGICD_PIDR2 - 1U):
			if (vgic_decode_irouter_reg(offset, &irouter_reg)) {
				mmio->value = vgic_read_irouter_word(vgic, &irouter_reg);
			} else {
				mmio->value = 0UL;
			}
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
	struct vgic_irq_config_reg config_reg;
	struct vgic_irouter_reg irouter_reg;
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
	} else if (vgic_decode_irq_config_reg(offset, false, &config_reg)) {
		vgic_write_irq_config(vcpu, config_reg.irq_base, value);
		flush = true;
	} else {
		switch (offset) {
		case VGICD_CTLR:
			vgic->gicd_ctlr = value & ~VGICD_CTLR_RWP;
			flush = true;
			break;
		case VGICD_ITARGETSR ... (VGICD_ICFGR - 1U):
			vgic_write_irq_target(vcpu, offset - VGICD_ITARGETSR,
				mmio->size, mmio->value);
			flush = true;
			break;
		case VGICD_IROUTER ... (VGICD_PIDR2 - 1U):
			if (vgic_decode_irouter_reg(offset, &irouter_reg)) {
				vgic_write_irouter_word(vcpu->vm, vgic, &irouter_reg, value);
				flush = true;
			}
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
		switch (mmio->direction) {
		case ACRN_IOREQ_DIR_READ:
			mmio->value = vgic_read_irq_priority(vcpu, irq_base, mmio->size);
			break;
		case ACRN_IOREQ_DIR_WRITE:
			vgic_write_irq_priority(vcpu, irq_base, mmio->size, mmio->value);
			vgicv3_flush_vm(vcpu);
			break;
		default:
			ret = -EINVAL;
			break;
		}
	} else if (vgic_target_access(offset, mmio->size)) {
		irq_base = offset - VGICD_ITARGETSR;
		switch (mmio->direction) {
		case ACRN_IOREQ_DIR_READ:
			mmio->value = vgic_read_irq_target(vcpu, irq_base, mmio->size);
			break;
		case ACRN_IOREQ_DIR_WRITE:
			vgic_write_irq_target(vcpu, irq_base, mmio->size, mmio->value);
			vgicv3_flush_vm(vcpu);
			break;
		default:
			ret = -EINVAL;
			break;
		}
	} else if (!vgic_word_access(offset, mmio->size)) {
		ret = -EINVAL;
	} else {
		switch (mmio->direction) {
		case ACRN_IOREQ_DIR_READ:
			vgicd_mmio_read(vcpu, vgic, mmio, offset);
			break;
		case ACRN_IOREQ_DIR_WRITE:
			if (vgicd_mmio_write(vcpu, vgic, mmio, offset)) {
				vgicv3_flush_vm(vcpu);
			}
			break;
		default:
			ret = -EINVAL;
			break;
		}
	}

	return ret;
}

static uint16_t vgicr_pcpu_id(const struct acrn_vcpu *vcpu, uint64_t addr)
{
	return (uint16_t)((addr - arm64_platform_guest_gicr_base(vcpu->vm->vm_id)) /
		arm64_platform_guest_gicr_stride(vcpu->vm->vm_id));
}

static uint32_t vgicr_offset(const struct acrn_vcpu *vcpu, uint64_t addr)
{
	return (uint32_t)((addr - arm64_platform_guest_gicr_base(vcpu->vm->vm_id)) %
		arm64_platform_guest_gicr_stride(vcpu->vm->vm_id));
}

static bool vgicr_decode_frame(const struct acrn_vcpu *vcpu, const struct arm64_vgicv3 *vgic,
	uint64_t addr, struct vgicr_frame *frame)
{
	uint16_t pcpu_id = vgicr_pcpu_id(vcpu, addr);
	bool ret = false;

	if ((pcpu_id < ARM64_VGIC_MAX_VCPUS) && (pcpu_id < vgic->rdist_count)) {
		frame->pcpu_id = pcpu_id;
		frame->vcpu_id = vgic->rdist_vcpu[pcpu_id];
		if ((frame->vcpu_id != VGIC_INVALID_VCPU_ID) &&
			(frame->vcpu_id >= vcpu->vm->hw.created_vcpus)) {
			frame->vcpu_id = VGIC_INVALID_VCPU_ID;
		}
		frame->offset = vgicr_offset(vcpu, addr);
		ret = true;
	}

	return ret;
}

static uint64_t vgicr_typer_value(const struct arm64_vgicv3 *vgic, const struct vgicr_frame *frame)
{
	uint64_t affinity = (frame->vcpu_id == VGIC_INVALID_VCPU_ID) ?
		VGIC_INVALID_AFFINITY : frame->vcpu_id;
	uint64_t typer = ((uint64_t)frame->pcpu_id << VGICR_TYPER_PROCNUM_SHIFT) |
		(affinity << VGICR_TYPER_AFF_SHIFT);

	if (vgic->its_enabled) {
		typer |= VGICR_TYPER_PLPIS;
	}
	if (frame->pcpu_id == (vgic->rdist_count - 1U)) {
		typer |= VGICR_TYPER_LAST;
	}

	return typer;
}

static void vgicr_write_waker(struct arm64_vgicv3 *vgic, uint16_t pcpu_id, uint32_t value)
{
	uint32_t waker = 0U;

	if ((value & VGICR_WAKER_PROCESSOR_SLEEP) != 0U) {
		waker = VGICR_WAKER_PROCESSOR_SLEEP | VGICR_WAKER_CHILDREN_ASLEEP;
	}

	vgic->gicr_waker[pcpu_id] = waker;
}

static void vgicr_sgi_mmio_read(struct acrn_vcpu *target_vcpu,
	struct acrn_mmio_request *mmio, uint32_t sgi_offset)
{
	struct vgic_irq_bit_reg bit_reg;
	struct vgic_irq_config_reg config_reg;
	uint32_t irq_base;

	/*
	 * The SGI/PPI sub-frame reuses distributor-style register offsets, but the
	 * current model exposes only the target vCPU's local INTID bank.
	 */
	if (vgic_decode_irq_bit_reg(sgi_offset, true, &bit_reg)) {
		mmio->value = vgic_read_irq_bits(target_vcpu, bit_reg.irq_base, bit_reg.selector);
	} else if (vgic_decode_irq_config_reg(sgi_offset, true, &config_reg)) {
		mmio->value = vgic_read_irq_config(target_vcpu, config_reg.irq_base);
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
	struct vgic_irq_config_reg config_reg;
	uint32_t irq_base;
	uint32_t value = (uint32_t)mmio->value;
	bool flush = false;

	if (vgic_decode_irq_bit_reg(sgi_offset, true, &bit_reg)) {
		vgic_write_irq_bits(target_vcpu, bit_reg.irq_base, bit_reg.selector, value);
		flush = true;
	} else if (vgic_decode_irq_config_reg(sgi_offset, true, &config_reg)) {
		vgic_write_irq_config(target_vcpu, config_reg.irq_base, value);
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

static void vgicr_mmio_read(struct arm64_vgicv3 *vgic,
	struct acrn_vcpu *target_vcpu, struct acrn_mmio_request *mmio,
	const struct vgicr_frame *frame)
{
	uint32_t offset = frame->offset;
	uint64_t typer;

	/*
	 * Guest redistributor frames are addressed by pCPU slot. A frame's
	 * GICR_TYPER still reports the guest vCPU affinity assigned to that pCPU
	 * slot, while SGI/PPI state is stored in the target vCPU's local IRQ row.
	 */
	switch (offset) {
	case VGICR_CTLR:
		mmio->value = vgic->gicr_ctlr[frame->pcpu_id] & ~VGICR_CTLR_RWP;
		break;
	case VGICR_IIDR:
		mmio->value = arm64_platform_gic_iidr();
		break;
	case VGICR_TYPER:
		typer = vgicr_typer_value(vgic, frame);
		mmio->value = typer & UINT32_MAX;
		break;
	case VGICR_TYPER + 4U:
		typer = vgicr_typer_value(vgic, frame);
		mmio->value = typer >> 32U;
		break;
	case VGICR_WAKER:
		mmio->value = vgic->gicr_waker[frame->pcpu_id] & VGICR_WAKER_READ_MASK;
		break;
	case VGICR_PROPBASER:
		mmio->value = vgic->gicr_propbaser[frame->pcpu_id] & UINT32_MAX;
		break;
	case VGICR_PROPBASER + 4U:
		mmio->value = vgic->gicr_propbaser[frame->pcpu_id] >> 32U;
		break;
	case VGICR_PENDBASER:
		mmio->value = vgic->gicr_pendbaser[frame->pcpu_id] & UINT32_MAX;
		break;
	case VGICR_PENDBASER + 4U:
		mmio->value = vgic->gicr_pendbaser[frame->pcpu_id] >> 32U;
		break;
	case VGICR_SYNCR:
		mmio->value = 0UL;
		break;
	case VGICR_PIDR2:
		mmio->value = GIC_PIDR2_ARCH_GICV3;
		break;
	case VGICR_SGI_BASE ... (VGICR_SGI_END - 1U):
		if (target_vcpu != NULL) {
			vgicr_sgi_mmio_read(target_vcpu, mmio, offset - VGICR_SGI_BASE);
		} else {
			mmio->value = 0UL;
		}
		break;
	default:
		mmio->value = 0UL;
		break;
	}
}

static bool vgicr_mmio_write(struct arm64_vgicv3 *vgic, struct acrn_vcpu *target_vcpu,
	const struct acrn_mmio_request *mmio, const struct vgicr_frame *frame)
{
	bool flush = false;

	switch (frame->offset) {
	case VGICR_CTLR:
		if (vgic->its_enabled) {
			vgic->gicr_ctlr[frame->pcpu_id] =
				(uint32_t)mmio->value & VGICR_CTLR_ENABLE_LPIS;
			flush = (target_vcpu != NULL);
		}
		break;
	case VGICR_WAKER:
		vgicr_write_waker(vgic, frame->pcpu_id, (uint32_t)mmio->value);
		break;
	case VGICR_PROPBASER:
		if (vgic->its_enabled) {
			vgic->gicr_propbaser[frame->pcpu_id] =
				(vgic->gicr_propbaser[frame->pcpu_id] & 0xffffffff00000000UL) |
				(mmio->value & UINT32_MAX);
		}
		break;
	case VGICR_PROPBASER + 4U:
		if (vgic->its_enabled) {
			vgic->gicr_propbaser[frame->pcpu_id] =
				(vgic->gicr_propbaser[frame->pcpu_id] & UINT32_MAX) |
				((mmio->value & UINT32_MAX) << 32U);
		}
		break;
	case VGICR_PENDBASER:
		if (vgic->its_enabled) {
			vgic->gicr_pendbaser[frame->pcpu_id] =
				(vgic->gicr_pendbaser[frame->pcpu_id] & 0xffffffff00000000UL) |
				(mmio->value & UINT32_MAX);
		}
		break;
	case VGICR_PENDBASER + 4U:
		if (vgic->its_enabled) {
			vgic->gicr_pendbaser[frame->pcpu_id] =
				(vgic->gicr_pendbaser[frame->pcpu_id] & UINT32_MAX) |
				((mmio->value & UINT32_MAX) << 32U);
		}
		break;
	case VGICR_INVLPIR:
	case VGICR_INVLPIR + 4U:
	case VGICR_INVALLR:
	case VGICR_INVALLR + 4U:
		break;
	case VGICR_SGI_BASE ... (VGICR_SGI_END - 1U):
		if (target_vcpu != NULL) {
			flush = vgicr_sgi_mmio_write(target_vcpu, mmio, frame->offset - VGICR_SGI_BASE);
		}
		break;
	default:
		break;
	}

	return flush;
}

static int32_t vgicr_mmio_access(struct acrn_vcpu *vcpu, struct arm64_vgicv3 *vgic,
	struct acrn_mmio_request *mmio)
{
	struct vgicr_frame frame;
	struct acrn_vcpu *target_vcpu;
	uint32_t sgi_offset;
	uint32_t irq_base;
	int32_t ret = 0;

	/*
	 * The redistributor has two access domains: a top-level per-pCPU frame and
	 * an SGI/PPI sub-frame for the vCPU currently assigned to that pCPU slot.
	 * The sub-frame inherits distributor-style priority byte access; other
	 * modeled redistributor registers are 32-bit words.
	 */
	if (!vgicr_decode_frame(vcpu, vgic, mmio->address, &frame)) {
		ret = -EINVAL;
	} else {
		target_vcpu = (frame.vcpu_id == VGIC_INVALID_VCPU_ID) ?
			NULL : vcpu_from_vid(vcpu->vm, frame.vcpu_id);
		if ((frame.offset >= VGICR_SGI_BASE) &&
			vgic_priority_access(frame.offset - VGICR_SGI_BASE, mmio->size)) {
			sgi_offset = frame.offset - VGICR_SGI_BASE;
			irq_base = sgi_offset - VGICD_IPRIORITYR;
			switch (mmio->direction) {
			case ACRN_IOREQ_DIR_READ:
				mmio->value = (target_vcpu != NULL) ?
					vgic_read_irq_priority(target_vcpu, irq_base, mmio->size) : 0UL;
				break;
			case ACRN_IOREQ_DIR_WRITE:
				if (target_vcpu != NULL) {
					vgic_write_irq_priority(target_vcpu, irq_base, mmio->size,
						mmio->value);
					vgicv3_flush_target_vcpu(vcpu, target_vcpu);
				}
				break;
			default:
				ret = -EINVAL;
				break;
			}
		} else if (!vgic_word_access(frame.offset, mmio->size)) {
			ret = -EINVAL;
		} else {
			switch (mmio->direction) {
			case ACRN_IOREQ_DIR_READ:
				vgicr_mmio_read(vgic, target_vcpu, mmio, &frame);
				break;
			case ACRN_IOREQ_DIR_WRITE:
				if (vgicr_mmio_write(vgic, target_vcpu, mmio, &frame)) {
					vgicv3_flush_target_vcpu(vcpu, target_vcpu);
				}
				break;
			default:
				ret = -EINVAL;
				break;
			}
		}
	}

	return ret;
}

static uint32_t vits_baser_index(uint32_t offset)
{
	return (offset - GITS_BASER) / 8U;
}

static bool vits_mmio_read(struct acrn_vcpu *vcpu, struct arm64_vgicv3 *vgic,
	struct acrn_mmio_request *mmio, uint32_t offset)
{
	struct arm64_vits *its = &vgic->its;
	uint64_t value64 = 0UL;
	bool flush = false;

	if ((offset >= GITS_BASER) &&
		(offset < (GITS_BASER + (ARM64_VGIC_ITS_BASER_NUM * 8U)))) {
		uint32_t idx = vits_baser_index(offset);

		value64 = its->baser[idx];
		mmio->value = ((offset & 0x4U) == 0U) ?
			(value64 & UINT32_MAX) : (value64 >> 32U);
		return false;
	}

	switch (offset) {
	case GITS_CTLR:
		mmio->value = its->ctlr;
		break;
	case GITS_IIDR:
		mmio->value = arm64_platform_gic_iidr();
		break;
	case GITS_TYPER:
		mmio->value = its->typer & UINT32_MAX;
		break;
	case GITS_TYPER + 4U:
		mmio->value = its->typer >> 32U;
		break;
	case GITS_MPIDR:
		mmio->value = vcpu_get_vmpidr(vcpu_from_vid(vcpu->vm, BSP_CPU_ID));
		break;
	case GITS_CBASER:
		mmio->value = its->cbaser & UINT32_MAX;
		break;
	case GITS_CBASER + 4U:
		mmio->value = its->cbaser >> 32U;
		break;
	case GITS_CWRITER:
		mmio->value = its->cwriter & UINT32_MAX;
		break;
	case GITS_CWRITER + 4U:
		mmio->value = its->cwriter >> 32U;
		break;
	case GITS_CREADR:
		flush = vits_process_command_queue(vcpu->vm, vgic);
		mmio->value = its->creadr & UINT32_MAX;
		break;
	case GITS_CREADR + 4U:
		flush = vits_process_command_queue(vcpu->vm, vgic);
		mmio->value = its->creadr >> 32U;
		break;
	case GITS_PIDR2:
		mmio->value = GIC_PIDR2_ARCH_GICV3;
		break;
	case GITS_CIDR0:
		mmio->value = 0x0dU;
		break;
	case GITS_CIDR1:
		mmio->value = 0xf0U;
		break;
	case GITS_CIDR2:
		mmio->value = 0x05U;
		break;
	case GITS_CIDR3:
		mmio->value = 0xb1U;
		break;
	default:
		mmio->value = 0UL;
		break;
	}

	return flush;
}

static bool vits_mmio_write(struct acrn_vcpu *vcpu, struct arm64_vgicv3 *vgic,
	const struct acrn_mmio_request *mmio, uint32_t offset)
{
	struct arm64_vits *its = &vgic->its;
	uint64_t value = mmio->value & UINT32_MAX;
	bool flush = false;

	if ((offset >= GITS_BASER) &&
		(offset < (GITS_BASER + (ARM64_VGIC_ITS_BASER_NUM * 8U)))) {
		uint32_t idx = vits_baser_index(offset);

		if ((offset & 0x4U) == 0U) {
			its->baser[idx] = (its->baser[idx] & 0xffffffff00000000UL) | value;
		} else {
			its->baser[idx] = (its->baser[idx] & UINT32_MAX) | (value << 32U);
		}
		return false;
	}

	switch (offset) {
	case GITS_CTLR:
		its->ctlr = ((uint32_t)value & GITS_CTLR_ENABLE) |
			(((value & GITS_CTLR_ENABLE) == 0U) ? GITS_CTLR_QUIESCENT : 0U);
		if ((its->ctlr & GITS_CTLR_ENABLE) != 0U) {
			flush = vits_process_command_queue(vcpu->vm, vgic);
		}
		break;
	case GITS_CBASER:
		its->cbaser = (its->cbaser & 0xffffffff00000000UL) | value;
		its->creadr = 0UL;
		its->cwriter = 0UL;
		break;
	case GITS_CBASER + 4U:
		its->cbaser = (its->cbaser & UINT32_MAX) | (value << 32U);
		its->creadr = 0UL;
		its->cwriter = 0UL;
		break;
	case GITS_CWRITER:
		its->cwriter = (its->cwriter & 0xffffffff00000000UL) | value;
		flush = vits_process_command_queue(vcpu->vm, vgic);
		break;
	case GITS_CWRITER + 4U:
		its->cwriter = (its->cwriter & UINT32_MAX) | (value << 32U);
		flush = vits_process_command_queue(vcpu->vm, vgic);
		break;
	case GITS_TRANSLATER:
		vits_write_translater(vcpu->vm, vgic, (uint32_t)value);
		flush = true;
		break;
	default:
		break;
	}

	return flush;
}

static int32_t vits_mmio_access(struct acrn_vcpu *vcpu, struct arm64_vgicv3 *vgic,
	struct acrn_mmio_request *mmio)
{
	uint32_t offset = (uint32_t)(mmio->address - arm64_platform_guest_its_base(vcpu->vm->vm_id));
	int32_t ret = 0;

	if (!vgic_word_access(offset, mmio->size)) {
		ret = -EINVAL;
	} else {
		switch (mmio->direction) {
		case ACRN_IOREQ_DIR_READ:
			if (vits_mmio_read(vcpu, vgic, mmio, offset)) {
				vgicv3_flush_vm(vcpu);
			}
			break;
		case ACRN_IOREQ_DIR_WRITE:
			if (vits_mmio_write(vcpu, vgic, mmio, offset)) {
				vgicv3_flush_vm(vcpu);
			}
			break;
		default:
			ret = -EINVAL;
			break;
		}
	}

	return ret;
}

static bool vits_mmio_access64(struct acrn_vcpu *vcpu, struct arm64_vgicv3 *vgic,
	struct acrn_mmio_request *mmio, int32_t *status)
{
	struct arm64_vits *its = &vgic->its;
	uint32_t offset = (uint32_t)(mmio->address - arm64_platform_guest_its_base(vcpu->vm->vm_id));
	bool handled = false;

	if (status != NULL) {
		*status = -ENODEV;
	}
	if ((offset == GITS_CWRITER) && (mmio->size == 8UL) &&
		(mmio->direction == ACRN_IOREQ_DIR_WRITE)) {
		handled = true;
		/*
		 * CWRITER is a 64-bit producer pointer. Update the full value before
		 * consuming commands so a split low/high emulation does not run the
		 * queue against a half-written writer offset.
		 */
		its->cwriter = mmio->value;
		if (vits_process_command_queue(vcpu->vm, vgic)) {
			vgicv3_flush_vm(vcpu);
		}
		if (status != NULL) {
			*status = 0;
		}
	}

	return handled;
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
	} else if (vgic->its_enabled &&
		addr_in_range(mmio->address, arm64_platform_guest_its_base(vcpu->vm->vm_id),
		arm64_platform_guest_its_size(vcpu->vm->vm_id))) {
		ret = vits_mmio_access(vcpu, vgic, mmio);
	}

	return ret;
}

static int32_t vgicv3_mmio_dispatch64(struct acrn_vcpu *vcpu, struct arm64_vgicv3 *vgic,
	struct acrn_mmio_request *mmio)
{
	struct acrn_mmio_request low = *mmio;
	struct acrn_mmio_request high = *mmio;
	int32_t ret;

	if (vgic->its_enabled &&
		addr_in_range(mmio->address, arm64_platform_guest_its_base(vcpu->vm->vm_id),
		arm64_platform_guest_its_size(vcpu->vm->vm_id)) &&
		vits_mmio_access64(vcpu, vgic, mmio, &ret)) {
		return ret;
	}

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

static int32_t vgicv3_mmio_access(struct acrn_vcpu *vcpu, struct arm64_vgicv3 *vgic,
	struct acrn_mmio_request *mmio)
{
	int32_t ret;

	switch (mmio->size) {
	case 1UL:
	case 2UL:
	case 4UL:
		ret = vgicv3_mmio_dispatch(vcpu, vgic, mmio);
		break;
	case 8UL:
		ret = vgicv3_mmio_dispatch64(vcpu, vgic, mmio);
		break;
	default:
		ret = -EINVAL;
		break;
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
	 *
	 *   data abort -> io_request -> vGIC lock -> sync
	 *        -> emulate GICD/GICR/GITS -> flush if LR state changed
	 */
	if ((vcpu != NULL) && (vgic != NULL)) {
		struct arm64_vgicv3_vcpu_ctx *ctx = &vcpu->arch.vgic;
		uint64_t old_lrs[ARM64_VGIC_MAX_LRS];
		uint64_t old_hcr;
		uint8_t old_used_lrs;

		spinlock_irqsave_obtain(&vgic->lock, &flags);
		old_used_lrs = ctx->used_lrs;
		old_hcr = ctx->hcr;
		(void)memcpy(old_lrs, ctx->lr, sizeof(old_lrs));
		vgicv3_sync_vcpu(vcpu, true);
		ret = vgicv3_mmio_access(vcpu, vgic, mmio);
		if (vgicv3_lrs_changed(ctx, old_lrs, old_used_lrs, old_hcr)) {
			vgicv3_flush_vcpu(vcpu, true);
		}
		spinlock_irqrestore_release(&vgic->lock, flags);
	}

	return ret;
}
