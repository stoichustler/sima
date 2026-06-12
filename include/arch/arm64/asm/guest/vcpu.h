/*
 * Copyright (C) 2026 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_GUEST_VCPU_H
#define ARM64_GUEST_VCPU_H

#include <types.h>
#include <asm/page.h>
#include <cpu.h>
#include <asm/guest/vgicv3.h>

#ifndef ASSEMBLER

#define ARM64_VCPU_REQUEST_EXCEPTION		0U
#define ARM64_VCPU_REQUEST_EVENT		1U

#define ARM64_VCPU_EVENT_VIRTUAL_INTERRUPT	0U

/*
 * EL2 control state that is programmed around vCPU scheduling. The guest GPRs
 * live in acrn_vcpu_arch::regs; this structure contains the translation,
 * execution-control, and timer-offset registers that define the EL1 virtual
 * CPU environment.
 *
 * When two VMs share one pCPU, EL1 state cannot be treated as pCPU-local
 * scratch state. Translation registers, exception registers, TPIDR values, and
 * generic-timer programming all belong to the vCPU that was running when the
 * guest left EL1. Saving them here prevents the next vCPU on the same pCPU
 * from inheriting another VM's address space, exception return state, or timer
 * deadline.
 */
struct arm64_vcpu_guest_ctx {
	uint64_t vttbr_el2;
	uint64_t vtcr_el2;
	uint64_t hcr_el2;
	uint64_t cntvoff_el2;
	uint64_t cntv_cval_el0;
	uint32_t cntv_ctl_el0;
	uint32_t timer_virq;
	uint64_t sctlr_el1;
	uint64_t ttbr0_el1;
	uint64_t ttbr1_el1;
	uint64_t tcr_el1;
	uint64_t mair_el1;
	uint64_t amair_el1;
	uint64_t vbar_el1;
	uint64_t contextidr_el1;
	uint64_t cpacr_el1;
	uint64_t tpidr_el0;
	uint64_t tpidrro_el0;
	uint64_t tpidr_el1;
	uint64_t sp_el0;
	uint64_t elr_el1;
	uint64_t spsr_el1;
	uint64_t esr_el1;
	uint64_t far_el1;
	uint64_t afsr0_el1;
	uint64_t afsr1_el1;
	uint64_t par_el1;
};

/*
 * Deferred trap injection state. A producer records the target exception frame
 * here and raises ARM64_VCPU_REQUEST_EXCEPTION; the vCPU thread consumes it
 * just before returning to the guest.
 */
struct arm64_vcpu_trap_info {
	uint64_t elr;
	uint64_t spsr;
	uint64_t esr;
	uint64_t far;
};

struct acrn_vcpu_arch {
	/*
	 * This must be the first member of acrn_vcpu_arch so low-level assembly
	 * can treat struct acrn_vcpu_arch as a struct cpu_regs when building the
	 * guest-entry frame.
	 */
	struct cpu_regs regs;

	struct arm64_vcpu_guest_ctx gctx;
	struct arm64_vgicv3_vcpu_ctx vgic;
	struct arm64_vcpu_trap_info trap;
	uint64_t irqs_pending;
	uint64_t irqs_pending_mask;
} __aligned(PAGE_SIZE);

struct acrn_vcpu;

int32_t arm64_process_vcpu_requests(struct acrn_vcpu *vcpu);
bool arm64_is_acrn_hypercall(uint64_t hcall_id);
int32_t arm64_dispatch_hypercall(struct acrn_vcpu *vcpu);

#endif /* ASSEMBLER */

#endif /* ARM64_GUEST_VCPU_H */
