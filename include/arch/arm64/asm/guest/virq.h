/*
 * Copyright (C) 2026 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_GUEST_VIRQ_H
#define ARM64_GUEST_VIRQ_H

#include <types.h>

struct acrn_vcpu;
struct arm64_vcpu_trap_info;

void vcpu_set_trap(struct acrn_vcpu *vcpu, struct arm64_vcpu_trap_info *trap);
void vcpu_queue_exception(struct acrn_vcpu *vcpu, struct arm64_vcpu_trap_info *trap);
int32_t vcpu_set_intr(struct acrn_vcpu *vcpu, uint32_t hwirq);
int32_t vcpu_clear_intr(struct acrn_vcpu *vcpu, uint32_t hwirq);
bool vcpu_inject_pending_intr(struct acrn_vcpu *vcpu);

static inline void vcpu_inject_extint(__unused struct acrn_vcpu *vcpu)
{
}

static inline void vcpu_inject_nmi(__unused struct acrn_vcpu *vcpu)
{
}

static inline void vcpu_inject_thermal_interrupt(__unused struct acrn_vcpu *vcpu)
{
}

#endif /* ARM64_GUEST_VIRQ_H */
