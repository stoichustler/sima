/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <cpu.h>
#include <per_cpu.h>
#include <io.h>
#include <logmsg.h>
#include <rtl.h>
#include <barrier.h>
#include <asm/irq.h>
#include <asm/platform.h>
#include <asm/sysreg.h>

/*
 * Physical GICv3 support for the EL2 host. This layer initializes the real
 * distributor, redistributors, and CPU interface, then exposes INTIDs through
 * the generic ACRN IRQ core. The virtual GIC model lives separately in
 * arch/arm64/guest/vgicv3.c.
 */
#define BIT32(n)			(1U << (n))

#define GICD_CTLR			0x0000U
#define GICD_TYPER			0x0004U
#define GICD_IGROUPR			0x0080U
#define GICD_ISENABLER			0x0100U
#define GICD_ICENABLER			0x0180U
#define GICD_ISPENDR			0x0200U
#define GICD_ICPENDR			0x0280U
#define GICD_ISACTIVER			0x0300U
#define GICD_ICACTIVER			0x0380U
#define GICD_IPRIORITYR		0x0400U
#define GICD_ICFGR			0x0C00U
#define GICD_IROUTER			0x6000U

#define GICD_CTLR_ENABLE_G1NS		BIT32(1U)
#define GICD_CTLR_ARE_NS		BIT32(5U)
#define GICD_CTLR_RWP			BIT32(31U)

#define GICR_CTLR			0x0000U
#define GICR_TYPER			0x0008U
#define GICR_WAKER			0x0014U
#define GICR_SGI_BASE			0x10000U

#define GICR_TYPER_LAST		(1UL << 4U)
#define GICR_TYPER_AFF_SHIFT		32U
#define GICR_TYPER_AFF_MASK		0xffffffffUL

#define GICR_WAKER_PROCESSOR_SLEEP	BIT32(1U)
#define GICR_WAKER_CHILDREN_ASLEEP	BIT32(2U)

#define GITS_CTLR			0x0000U
#define GITS_TYPER			0x0008U
#define GITS_CTLR_ENABLE		BIT32(0U)
#define GITS_CTLR_QUIESCENT		BIT32(31U)

#define GIC_INTIDS_PER_REG		32U
#define GIC_PRI_PER_REG		4U
#define GIC_CFG_PER_REG		16U
#define GIC_SPI_BASE			32U
#define GIC_DEFAULT_PRIORITY		0x80U
#define GIC_LOWEST_PRIORITY		0xffU

static uint64_t gicr_base_by_pcpu[MAX_PCPU_NUM];
static uint32_t gic_line_count;
static bool gicv3_global_initialized;
static bool gicv3_its_present;

static inline void *gicd_addr(uint32_t off)
{
	return (void *)(arm64_platform_gicd_base() + off);
}

static inline void *gicr_addr(uint64_t base, uint32_t off)
{
	return (void *)(base + off);
}

static inline uint32_t gicd_read32(uint32_t off)
{
	return mmio_read32(gicd_addr(off));
}

static inline void gicd_write32(uint32_t off, uint32_t val)
{
	mmio_write32(val, gicd_addr(off));
}

static inline void gicd_write64(uint32_t off, uint64_t val)
{
	mmio_write64(val, gicd_addr(off));
}

static inline uint32_t gicr_read32(uint64_t base, uint32_t off)
{
	return mmio_read32(gicr_addr(base, off));
}

static inline uint64_t gicr_read64(uint64_t base, uint32_t off)
{
	return mmio_read64(gicr_addr(base, off));
}

static inline void gicr_write32(uint64_t base, uint32_t off, uint32_t val)
{
	mmio_write32(val, gicr_addr(base, off));
}

static inline void *gits_addr(uint32_t off)
{
	return (void *)(arm64_platform_gits_base() + off);
}

static inline uint32_t gits_read32(uint32_t off)
{
	return mmio_read32(gits_addr(off));
}

static inline uint64_t gits_read64(uint32_t off)
{
	return mmio_read64(gits_addr(off));
}

static inline void gits_write32(uint32_t off, uint32_t val)
{
	mmio_write32(val, gits_addr(off));
}

static uint32_t gic_intid_reg(uint32_t intid)
{
	return (intid / GIC_INTIDS_PER_REG) * 4U;
}

static uint32_t gic_intid_mask(uint32_t intid)
{
	return BIT32(intid % GIC_INTIDS_PER_REG);
}

static uint64_t gic_current_rdist(void)
{
	uint16_t pcpu_id = get_pcpu_id();

	if (pcpu_id >= MAX_PCPU_NUM) {
		panic("invalid pcpu%hu for gic redistributor", pcpu_id);
	}

	return gicr_base_by_pcpu[pcpu_id];
}

static void gicd_wait_rwp(void)
{
	while ((gicd_read32(GICD_CTLR) & GICD_CTLR_RWP) != 0U) {
		cpu_relax();
	}
}

static void gicr_wait_rwp(uint64_t rdist)
{
	while ((gicr_read32(rdist, GICR_CTLR) & BIT32(3U)) != 0U) {
		cpu_relax();
	}
}

static uint64_t gic_mpidr_to_affinity(uint64_t mpidr)
{
	uint64_t aff0 = mpidr & 0xffUL;
	uint64_t aff1 = (mpidr >> 8U) & 0xffUL;
	uint64_t aff2 = (mpidr >> 16U) & 0xffUL;
	uint64_t aff3 = (mpidr >> 32U) & 0xffUL;

	return aff0 | (aff1 << 8U) | (aff2 << 16U) | (aff3 << 24U);
}

static void gic_discover_rdists(void)
{
	uint64_t rdist = arm64_platform_gicr_base();
	uint32_t frame;

	/*
	 * Redistributor frames are discovered by matching the GICR_TYPER affinity
	 * value to each pCPU's MPIDR. This avoids assuming that frame order and
	 * logical pCPU IDs are identical.
	 */
	for (frame = 0U; frame < MAX_PCPU_NUM; frame++) {
		uint64_t typer = gicr_read64(rdist, GICR_TYPER);
		uint64_t aff = (typer >> GICR_TYPER_AFF_SHIFT) & GICR_TYPER_AFF_MASK;
		uint16_t pcpu_id;

		for (pcpu_id = 0U; pcpu_id < MAX_PCPU_NUM; pcpu_id++) {
			if (gic_mpidr_to_affinity(per_cpu(arch.mpidr, pcpu_id)) == aff) {
				gicr_base_by_pcpu[pcpu_id] = rdist;
				break;
			}
		}

		if ((typer & GICR_TYPER_LAST) != 0UL) {
			break;
		}

		rdist += arm64_platform_gicr_stride();
	}
}

static void gicd_set_priority(uint32_t intid, uint8_t priority)
{
	mmio_write8(priority, gicd_addr(GICD_IPRIORITYR + intid));
}

static void gicr_set_priority(uint64_t rdist, uint32_t intid, uint8_t priority)
{
	mmio_write8(priority, gicr_addr(rdist, GICR_SGI_BASE + GICD_IPRIORITYR + intid));
}

static void gicd_clear_pending_active(uint32_t intid)
{
	gicd_write32(GICD_ICPENDR + gic_intid_reg(intid), gic_intid_mask(intid));
	gicd_write32(GICD_ICACTIVER + gic_intid_reg(intid), gic_intid_mask(intid));
}

static void gicr_enable_irq(uint64_t rdist, uint32_t intid)
{
	gicr_write32(rdist, GICR_SGI_BASE + GICD_ISENABLER + gic_intid_reg(intid),
		gic_intid_mask(intid));
}

static void gicr_disable_irq(uint64_t rdist, uint32_t intid)
{
	gicr_write32(rdist, GICR_SGI_BASE + GICD_ICENABLER + gic_intid_reg(intid),
		gic_intid_mask(intid));
}

static void gicr_clear_pending(uint64_t rdist, uint32_t intid)
{
	gicr_write32(rdist, GICR_SGI_BASE + GICD_ICPENDR + gic_intid_reg(intid),
		gic_intid_mask(intid));
}

static void gicr_clear_pending_active(uint64_t rdist, uint32_t intid)
{
	gicr_write32(rdist, GICR_SGI_BASE + GICD_ICPENDR + gic_intid_reg(intid),
		gic_intid_mask(intid));
	gicr_write32(rdist, GICR_SGI_BASE + GICD_ICACTIVER + gic_intid_reg(intid),
		gic_intid_mask(intid));
}

static void gicd_init(void)
{
	uint32_t typer;
	uint32_t i;

	/*
	 * Bring the distributor to a known non-secure Group-1 state. SPIs are
	 * disabled, pending/active state is cleared, priorities are normalized, and
	 * routes default to the BSP until a fuller interrupt-routing policy exists.
	 */
	gicd_write32(GICD_CTLR, 0U);
	gicd_wait_rwp();

	typer = gicd_read32(GICD_TYPER);
	gic_line_count = (((typer & 0x1fU) + 1U) * GIC_INTIDS_PER_REG);
	if (gic_line_count > IRQ_NUM_GIC_DOMAIN) {
		gic_line_count = IRQ_NUM_GIC_DOMAIN;
	}

	for (i = GIC_SPI_BASE; i < gic_line_count; i += GIC_INTIDS_PER_REG) {
		gicd_write32(GICD_ICENABLER + gic_intid_reg(i), UINT32_MAX);
		gicd_write32(GICD_ICPENDR + gic_intid_reg(i), UINT32_MAX);
		gicd_write32(GICD_ICACTIVER + gic_intid_reg(i), UINT32_MAX);
		gicd_write32(GICD_IGROUPR + gic_intid_reg(i), UINT32_MAX);
	}

	for (i = GIC_SPI_BASE; i < gic_line_count; i++) {
		gicd_set_priority(i, GIC_DEFAULT_PRIORITY);
		gicd_write64(GICD_IROUTER + (i * 8U), per_cpu(arch.mpidr, BSP_CPU_ID));
	}

	for (i = GIC_SPI_BASE; i < gic_line_count; i += GIC_CFG_PER_REG) {
		gicd_write32(GICD_ICFGR + ((i / GIC_CFG_PER_REG) * 4U), 0U);
	}

	gicd_write32(GICD_CTLR, GICD_CTLR_ARE_NS | GICD_CTLR_ENABLE_G1NS);
	gicd_wait_rwp();
}

static void gits_init(void)
{
	uint64_t base = arm64_platform_gits_base();
	uint64_t size = arm64_platform_gits_size();
	uint32_t ctlr;

	if ((base == 0UL) || (size == 0UL)) {
		gicv3_its_present = false;
		return;
	}

	gits_write32(GITS_CTLR, 0U);
	do {
		ctlr = gits_read32(GITS_CTLR);
	} while ((ctlr & GITS_CTLR_QUIESCENT) == 0U);

	gicv3_its_present = true;
	pr_info("gicv3 its: base=0x%lx size=0x%lx typer=0x%lx",
		base, size, gits_read64(GITS_TYPER));
}

static void gicr_wake(uint64_t rdist)
{
	uint32_t val = gicr_read32(rdist, GICR_WAKER);

	val &= ~GICR_WAKER_PROCESSOR_SLEEP;
	gicr_write32(rdist, GICR_WAKER, val);

	while ((gicr_read32(rdist, GICR_WAKER) & GICR_WAKER_CHILDREN_ASLEEP) != 0U) {
		cpu_relax();
	}
}

static void gicr_init(uint64_t rdist)
{
	uint32_t i;

	/*
	 * Each CPU programs its own redistributor for SGIs/PPIs. The host enables
	 * the SMP-call SGI, the vGIC maintenance PPI, and the virtual-timer PPI
	 * because those are required to run and schedule guests.
	 */
	gicr_wake(rdist);

	gicr_write32(rdist, GICR_SGI_BASE + GICD_ICENABLER, UINT32_MAX);
	gicr_wait_rwp(rdist);
	gicr_write32(rdist, GICR_SGI_BASE + GICD_ICPENDR, UINT32_MAX);
	gicr_write32(rdist, GICR_SGI_BASE + GICD_ICACTIVER, UINT32_MAX);
	gicr_write32(rdist, GICR_SGI_BASE + GICD_IGROUPR, UINT32_MAX);

	for (i = 0U; i < GIC_SPI_BASE; i++) {
		gicr_set_priority(rdist, i, GIC_DEFAULT_PRIORITY);
	}

	gicr_enable_irq(rdist, ARM64_GIC_SGI_SMP_CALL);
	gicr_enable_irq(rdist, ARM64_GIC_PPI_VGIC_MAINTENANCE);
	gicr_enable_irq(rdist, ARM64_GIC_PPI_VIRTUAL_TIMER);
	gicr_wait_rwp(rdist);
}

static void gic_cpuif_init(void)
{
	/*
	 * Enable system-register access to the GIC CPU interface. EL2 keeps SRE
	 * enabled and permits EL1 SRE so guests can use ICC_* traps/emulation for
	 * the subset currently virtualized.
	 */
	write_icc_sre_el2(ICC_SRE_ENABLE | ICC_SRE_DIB | ICC_SRE_DFB | ICC_SRE_SRE);
	write_icc_sre_el1(ICC_SRE_SRE);
	write_icc_pmr_el1(GIC_LOWEST_PRIORITY);
	write_icc_bpr1_el1(0U);
	write_icc_ctlr_el1(0U);
	write_icc_igrpen1_el1(1UL);
}

void arm64_gicv3_init_early(void)
{
	if (!gicv3_global_initialized) {
		gic_discover_rdists();
		gicd_init();
		gits_init();
		gicv3_global_initialized = true;
	}
}

bool arm64_gicv3_has_its(void)
{
	return gicv3_its_present;
}

void arm64_gicv3_init(uint16_t pcpu_id)
{
	uint64_t rdist;

	/*
	 * Global distributor setup is BSP-only. Redistributor and CPU-interface
	 * setup is per-pCPU and must run after the pCPU has its MPIDR recorded.
	 */
	if (pcpu_id == BSP_CPU_ID) {
		arm64_gicv3_init_early();
	}

	rdist = gicr_base_by_pcpu[pcpu_id];
	if (rdist == 0UL) {
		panic("no gic redistributor for pcpu%hu mpidr=0x%lx", pcpu_id,
			per_cpu(arch.mpidr, pcpu_id));
	}

	gicr_init(rdist);
	gic_cpuif_init();
}

uint32_t arm64_gicv3_ack_irq(void)
{
	return (uint32_t)(read_icc_iar1_el1() & 0x00ffffffU);
}

void arm64_gicv3_eoi_irq(uint32_t intid)
{
	cpu_memory_barrier();
	write_icc_eoir1_el1(intid);
}

void arm64_gicv3_enable_irq(uint32_t intid)
{
	uint64_t rdist;

	/*
	 * SGIs/PPIs are configured in the current CPU redistributor; SPIs are
	 * configured in the distributor. The generic IRQ core passes physical GIC
	 * INTIDs here after mapping them through the ARM64 IRQ domain.
	 */
	if (intid < GIC_SPI_BASE) {
		rdist = gic_current_rdist();
		gicr_clear_pending_active(rdist, intid);
		gicr_enable_irq(rdist, intid);
		gicr_wait_rwp(rdist);
	} else if (intid < gic_line_count) {
		gicd_clear_pending_active(intid);
		gicd_write32(GICD_ISENABLER + gic_intid_reg(intid), gic_intid_mask(intid));
		gicd_wait_rwp();
	}
}

void arm64_gicv3_disable_irq(uint32_t intid)
{
	uint64_t rdist;

	if (intid < GIC_SPI_BASE) {
		rdist = gic_current_rdist();
		gicr_disable_irq(rdist, intid);
		gicr_wait_rwp(rdist);
	} else if (intid < gic_line_count) {
		gicd_write32(GICD_ICENABLER + gic_intid_reg(intid), gic_intid_mask(intid));
		gicd_wait_rwp();
	}
}

void arm64_gicv3_clear_irq(uint32_t intid)
{
	uint64_t rdist;

	if (intid < GIC_SPI_BASE) {
		rdist = gic_current_rdist();
		gicr_clear_pending(rdist, intid);
		gicr_write32(rdist, GICR_SGI_BASE + GICD_ICACTIVER + gic_intid_reg(intid),
			gic_intid_mask(intid));
	} else if (intid < gic_line_count) {
		gicd_clear_pending_active(intid);
	}
}

void arm64_gicv3_get_local_irq_state(uint16_t pcpu_id, uint32_t intid,
	struct arm64_gicv3_local_irq_state *state)
{
	uint64_t rdist;
	uint32_t reg;
	uint32_t mask;

	if (state == NULL) {
		return;
	}

	(void)memset(state, 0U, sizeof(*state));
	if ((pcpu_id >= MAX_PCPU_NUM) || (intid >= GIC_SPI_BASE)) {
		return;
	}

	rdist = gicr_base_by_pcpu[pcpu_id];
	if (rdist == 0UL) {
		return;
	}

	reg = gic_intid_reg(intid);
	mask = gic_intid_mask(intid);
	state->enabled = gicr_read32(rdist, GICR_SGI_BASE + GICD_ISENABLER + reg) & mask;
	state->pending = gicr_read32(rdist, GICR_SGI_BASE + GICD_ISPENDR + reg) & mask;
	state->active = gicr_read32(rdist, GICR_SGI_BASE + GICD_ISACTIVER + reg) & mask;
	state->group = gicr_read32(rdist, GICR_SGI_BASE + GICD_IGROUPR + reg) & mask;
	state->priority = mmio_read8(gicr_addr(rdist, GICR_SGI_BASE + GICD_IPRIORITYR + intid));
	state->valid = true;
}

void arm64_gicv3_send_sgi(uint16_t pcpu_id, uint32_t sgi_id)
{
	uint64_t mpidr;
	uint64_t aff1;
	uint64_t aff2;
	uint64_t aff3;
	uint64_t target_list;
	uint64_t sgi;

	if ((pcpu_id >= MAX_PCPU_NUM) || (sgi_id >= 16U)) {
		return;
	}

	/*
	 * ICC_SGI1R_EL1 targets by affinity. Use the destination pCPU MPIDR so SGI
	 * delivery remains correct even if redistributor frame order differs from
	 * logical pCPU numbering.
	 */
	mpidr = per_cpu(arch.mpidr, pcpu_id);
	aff1 = (mpidr >> 8U) & 0xffUL;
	aff2 = (mpidr >> 16U) & 0xffUL;
	aff3 = (mpidr >> 32U) & 0xffUL;
	target_list = 1UL << (mpidr & 0xfUL);

	sgi = (aff3 << 48U) | (aff2 << 32U) | (sgi_id << 24U) | (aff1 << 16U) | target_list;
	cpu_memory_barrier();
	write_icc_sgi1r_el1(sgi);
}
