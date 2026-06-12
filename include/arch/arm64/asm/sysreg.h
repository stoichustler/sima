/*
 * Copyright (C) 2026 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_SYSREG_H
#define ARM64_SYSREG_H

#include <types.h>

#define MPIDR_AFFINITY_MASK	0x00FFFFFFUL

#define CNTV_CTL_ENABLE		(1U << 0U)
#define CNTV_CTL_IMASK		(1U << 1U)
#define CNTV_CTL_ISTATUS	(1U << 2U)

#define CNTHCTL_EL2_EL1PCTEN	(1UL << 0U)
#define CNTHCTL_EL2_EL1PCEN	(1UL << 1U)
#define CNTHCTL_EL2_EL1VCTEN	(1UL << 8U)
#define CNTHCTL_EL2_EL1VTEN	(1UL << 9U)

#define HCR_VM			(1UL << 0U)
#define HCR_IMO			(1UL << 4U)
#define HCR_FMO			(1UL << 3U)
#define HCR_AMO			(1UL << 5U)
#define HCR_RW			(1UL << 31U)
#define HCR_VI			(1UL << 7U)
#define HCR_VF			(1UL << 6U)
#define HCR_TSC			(1UL << 19U)

#define ICH_HCR_EN		(1UL << 0U)
#define ICH_HCR_UIE		(1UL << 1U)
#define ICH_HCR_LRENPIE		(1UL << 2U)
#define ICH_HCR_NPIE		(1UL << 3U)

#define ICH_VMCR_VENG1		(1UL << 1U)
#define ICH_VMCR_DEFAULT_MASK	(0xf8UL << 24U)

#define ICH_LR_VINTID_MASK	0xffffffffUL
#define ICH_LR_PINTID_SHIFT	32U
#define ICH_LR_PRIORITY_SHIFT	48U
#define ICH_LR_GROUP1		(1UL << 60U)
#define ICH_LR_HW		(1UL << 61U)
#define ICH_LR_STATE_SHIFT	62U
#define ICH_LR_STATE_INVALID	0UL
#define ICH_LR_STATE_PENDING	1UL
#define ICH_LR_STATE_ACTIVE	2UL
#define ICH_LR_STATE_ACTIVE_PENDING 3UL

#define ICC_CTLR_EL1_EOIMODE	(1UL << 1U)
#define ICC_SRE_SRE		(1UL << 0U)
#define ICC_SRE_DFB		(1UL << 1U)
#define ICC_SRE_DIB		(1UL << 2U)
#define ICC_SRE_ENABLE		(1UL << 3U)

#ifndef ASSEMBLER

static inline uint64_t read_mpidr_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, mpidr_el1" : "=r" (val));
	return val;
}

static inline uint64_t read_currentel(void)
{
	uint64_t val;

	asm volatile ("mrs %0, CurrentEL" : "=r" (val));
	return val;
}

static inline uint64_t read_cntfrq_el0(void)
{
	uint64_t val;

	asm volatile ("mrs %0, cntfrq_el0" : "=r" (val));
	return val;
}

static inline uint64_t read_cntpct_el0(void)
{
	uint64_t val;

	asm volatile ("mrs %0, cntpct_el0" : "=r" (val));
	return val;
}

static inline uint64_t read_cntvct_el0(void)
{
	uint64_t val;

	asm volatile ("mrs %0, cntvct_el0" : "=r" (val));
	return val;
}

static inline void write_cntv_tval_el0(uint32_t val)
{
	asm volatile ("msr cntv_tval_el0, %0" : : "r" (val));
}

static inline uint64_t read_cntv_cval_el0(void)
{
	uint64_t val;

	asm volatile ("mrs %0, cntv_cval_el0" : "=r" (val));
	return val;
}

static inline void write_cntv_cval_el0(uint64_t val)
{
	asm volatile ("msr cntv_cval_el0, %0" : : "r" (val));
}

static inline uint32_t read_cntv_ctl_el0(void)
{
	uint64_t val;

	asm volatile ("mrs %0, cntv_ctl_el0" : "=r" (val));
	return (uint32_t)val;
}

static inline void write_cntv_ctl_el0(uint32_t val)
{
	asm volatile ("msr cntv_ctl_el0, %0" : : "r" (val));
}

static inline void write_cnthctl_el2(uint64_t val)
{
	asm volatile ("msr cnthctl_el2, %0; isb" : : "r" (val) : "memory");
}

static inline void write_cntp_tval_el0(uint32_t val)
{
	asm volatile ("msr cntp_tval_el0, %0" : : "r" (val));
}

static inline void write_cntp_ctl_el0(uint32_t val)
{
	asm volatile ("msr cntp_ctl_el0, %0" : : "r" (val));
}

static inline void write_icc_sgi1r_el1(uint64_t val)
{
	asm volatile ("msr s3_0_c12_c11_5, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_icc_iar1_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_0_c12_c12_0" : "=r" (val));
	return val;
}

static inline void write_icc_eoir1_el1(uint64_t val)
{
	asm volatile ("msr s3_0_c12_c12_1, %0; isb" : : "r" (val) : "memory");
}

static inline void write_icc_pmr_el1(uint64_t val)
{
	asm volatile ("msr s3_0_c4_c6_0, %0; isb" : : "r" (val) : "memory");
}

static inline void write_icc_bpr1_el1(uint64_t val)
{
	asm volatile ("msr s3_0_c12_c12_3, %0; isb" : : "r" (val) : "memory");
}

static inline void write_icc_ctlr_el1(uint64_t val)
{
	asm volatile ("msr s3_0_c12_c12_4, %0; isb" : : "r" (val) : "memory");
}

static inline void write_icc_igrpen1_el1(uint64_t val)
{
	asm volatile ("msr s3_0_c12_c12_7, %0; isb" : : "r" (val) : "memory");
}

static inline void write_icc_sre_el2(uint64_t val)
{
	asm volatile ("msr s3_4_c12_c9_5, %0; isb" : : "r" (val) : "memory");
}

static inline void write_icc_sre_el1(uint64_t val)
{
	asm volatile ("msr s3_0_c12_c12_5, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_icc_sre_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_0_c12_c12_5" : "=r" (val));
	return val;
}

static inline uint64_t read_icc_pmr_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_0_c4_c6_0" : "=r" (val));
	return val;
}

static inline uint64_t read_icc_ctlr_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_0_c12_c12_4" : "=r" (val));
	return val;
}

static inline uint64_t read_ich_vtr_el2(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_4_c12_c11_1" : "=r" (val));
	return val;
}

static inline uint64_t read_ich_hcr_el2(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_4_c12_c11_0" : "=r" (val));
	return val;
}

static inline void write_ich_hcr_el2(uint64_t val)
{
	asm volatile ("msr s3_4_c12_c11_0, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_ich_vmcr_el2(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_4_c12_c11_7" : "=r" (val));
	return val;
}

static inline void write_ich_vmcr_el2(uint64_t val)
{
	asm volatile ("msr s3_4_c12_c11_7, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_ich_eisr_el2(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_4_c12_c11_3" : "=r" (val));
	return val;
}

static inline uint64_t read_ich_elrsr_el2(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_4_c12_c11_5" : "=r" (val));
	return val;
}

static inline uint64_t read_ich_lr_el2(uint8_t idx)
{
	uint64_t val = 0UL;

	switch (idx) {
	case 0U:
		asm volatile ("mrs %0, s3_4_c12_c12_0" : "=r" (val));
		break;
	case 1U:
		asm volatile ("mrs %0, s3_4_c12_c12_1" : "=r" (val));
		break;
	case 2U:
		asm volatile ("mrs %0, s3_4_c12_c12_2" : "=r" (val));
		break;
	case 3U:
		asm volatile ("mrs %0, s3_4_c12_c12_3" : "=r" (val));
		break;
	case 4U:
		asm volatile ("mrs %0, s3_4_c12_c12_4" : "=r" (val));
		break;
	case 5U:
		asm volatile ("mrs %0, s3_4_c12_c12_5" : "=r" (val));
		break;
	case 6U:
		asm volatile ("mrs %0, s3_4_c12_c12_6" : "=r" (val));
		break;
	case 7U:
		asm volatile ("mrs %0, s3_4_c12_c12_7" : "=r" (val));
		break;
	default:
		break;
	}

	return val;
}

static inline void write_ich_lr_el2(uint8_t idx, uint64_t val)
{
	switch (idx) {
	case 0U:
		asm volatile ("msr s3_4_c12_c12_0, %0; isb" : : "r" (val) : "memory");
		break;
	case 1U:
		asm volatile ("msr s3_4_c12_c12_1, %0; isb" : : "r" (val) : "memory");
		break;
	case 2U:
		asm volatile ("msr s3_4_c12_c12_2, %0; isb" : : "r" (val) : "memory");
		break;
	case 3U:
		asm volatile ("msr s3_4_c12_c12_3, %0; isb" : : "r" (val) : "memory");
		break;
	case 4U:
		asm volatile ("msr s3_4_c12_c12_4, %0; isb" : : "r" (val) : "memory");
		break;
	case 5U:
		asm volatile ("msr s3_4_c12_c12_5, %0; isb" : : "r" (val) : "memory");
		break;
	case 6U:
		asm volatile ("msr s3_4_c12_c12_6, %0; isb" : : "r" (val) : "memory");
		break;
	case 7U:
		asm volatile ("msr s3_4_c12_c12_7, %0; isb" : : "r" (val) : "memory");
		break;
	default:
		break;
	}
}

static inline uint64_t read_ich_ap0r0_el2(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_4_c12_c8_0" : "=r" (val));
	return val;
}

static inline void write_ich_ap0r0_el2(uint64_t val)
{
	asm volatile ("msr s3_4_c12_c8_0, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_ich_ap1r0_el2(void)
{
	uint64_t val;

	asm volatile ("mrs %0, s3_4_c12_c9_0" : "=r" (val));
	return val;
}

static inline void write_ich_ap1r0_el2(uint64_t val)
{
	asm volatile ("msr s3_4_c12_c9_0, %0; isb" : : "r" (val) : "memory");
}

static inline void write_hcr_el2(uint64_t val)
{
	asm volatile ("msr hcr_el2, %0; isb" : : "r" (val) : "memory");
}

static inline void write_vtcr_el2(uint64_t val)
{
	asm volatile ("msr vtcr_el2, %0; isb" : : "r" (val) : "memory");
}

static inline void write_vttbr_el2(uint64_t val)
{
	asm volatile ("msr vttbr_el2, %0; isb" : : "r" (val) : "memory");
}

static inline void write_vmpidr_el2(uint64_t val)
{
	asm volatile ("msr vmpidr_el2, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_sctlr_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, sctlr_el1" : "=r" (val));
	return val;
}

static inline void write_sctlr_el1(uint64_t val)
{
	asm volatile ("msr sctlr_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_ttbr0_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, ttbr0_el1" : "=r" (val));
	return val;
}

static inline void write_ttbr0_el1(uint64_t val)
{
	asm volatile ("msr ttbr0_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_ttbr1_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, ttbr1_el1" : "=r" (val));
	return val;
}

static inline void write_ttbr1_el1(uint64_t val)
{
	asm volatile ("msr ttbr1_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_tcr_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, tcr_el1" : "=r" (val));
	return val;
}

static inline void write_tcr_el1(uint64_t val)
{
	asm volatile ("msr tcr_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_mair_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, mair_el1" : "=r" (val));
	return val;
}

static inline void write_mair_el1(uint64_t val)
{
	asm volatile ("msr mair_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_amair_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, amair_el1" : "=r" (val));
	return val;
}

static inline void write_amair_el1(uint64_t val)
{
	asm volatile ("msr amair_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_vbar_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, vbar_el1" : "=r" (val));
	return val;
}

static inline void write_vbar_el1(uint64_t val)
{
	asm volatile ("msr vbar_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_contextidr_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, contextidr_el1" : "=r" (val));
	return val;
}

static inline void write_contextidr_el1(uint64_t val)
{
	asm volatile ("msr contextidr_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_cpacr_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, cpacr_el1" : "=r" (val));
	return val;
}

static inline void write_cpacr_el1(uint64_t val)
{
	asm volatile ("msr cpacr_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_tpidr_el0(void)
{
	uint64_t val;

	asm volatile ("mrs %0, tpidr_el0" : "=r" (val));
	return val;
}

static inline void write_tpidr_el0(uint64_t val)
{
	asm volatile ("msr tpidr_el0, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_tpidrro_el0(void)
{
	uint64_t val;

	asm volatile ("mrs %0, tpidrro_el0" : "=r" (val));
	return val;
}

static inline void write_tpidrro_el0(uint64_t val)
{
	asm volatile ("msr tpidrro_el0, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_tpidr_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, tpidr_el1" : "=r" (val));
	return val;
}

static inline void write_tpidr_el1(uint64_t val)
{
	asm volatile ("msr tpidr_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_sp_el0(void)
{
	uint64_t val;

	asm volatile ("mrs %0, sp_el0" : "=r" (val));
	return val;
}

static inline void write_sp_el0(uint64_t val)
{
	asm volatile ("msr sp_el0, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_elr_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, elr_el1" : "=r" (val));
	return val;
}

static inline void write_elr_el1(uint64_t val)
{
	asm volatile ("msr elr_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_spsr_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, spsr_el1" : "=r" (val));
	return val;
}

static inline void write_spsr_el1(uint64_t val)
{
	asm volatile ("msr spsr_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_esr_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, esr_el1" : "=r" (val));
	return val;
}

static inline void write_esr_el1(uint64_t val)
{
	asm volatile ("msr esr_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_far_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, far_el1" : "=r" (val));
	return val;
}

static inline void write_far_el1(uint64_t val)
{
	asm volatile ("msr far_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_afsr0_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, afsr0_el1" : "=r" (val));
	return val;
}

static inline void write_afsr0_el1(uint64_t val)
{
	asm volatile ("msr afsr0_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_afsr1_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, afsr1_el1" : "=r" (val));
	return val;
}

static inline void write_afsr1_el1(uint64_t val)
{
	asm volatile ("msr afsr1_el1, %0; isb" : : "r" (val) : "memory");
}

static inline uint64_t read_par_el1(void)
{
	uint64_t val;

	asm volatile ("mrs %0, par_el1" : "=r" (val));
	return val;
}

static inline void write_par_el1(uint64_t val)
{
	asm volatile ("msr par_el1, %0; isb" : : "r" (val) : "memory");
}

static inline void write_vbar_el2(uint64_t val)
{
	asm volatile ("msr vbar_el2, %0; isb" : : "r" (val) : "memory");
}

static inline void write_ttbr0_el2(uint64_t val)
{
	asm volatile ("msr ttbr0_el2, %0" : : "r" (val) : "memory");
}

static inline void write_tcr_el2(uint64_t val)
{
	asm volatile ("msr tcr_el2, %0" : : "r" (val) : "memory");
}

static inline void write_mair_el2(uint64_t val)
{
	asm volatile ("msr mair_el2, %0" : : "r" (val) : "memory");
}

static inline uint64_t read_sctlr_el2(void)
{
	uint64_t val;

	asm volatile ("mrs %0, sctlr_el2" : "=r" (val));
	return val;
}

static inline void write_sctlr_el2(uint64_t val)
{
	asm volatile ("msr sctlr_el2, %0; isb" : : "r" (val) : "memory");
}

static inline void flush_tlb_local(void)
{
	asm volatile ("dsb ishst; tlbi alle2; dsb ish; isb" ::: "memory");
}

static inline void flush_stage2_tlb_local(void)
{
	asm volatile ("dsb ishst; tlbi vmalls12e1; dsb ish; isb" ::: "memory");
}

#endif /* ASSEMBLER */

#endif /* ARM64_SYSREG_H */
