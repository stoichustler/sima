/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_GUEST_VCPU_PRIV_H
#define ARM64_GUEST_VCPU_PRIV_H

#include <types.h>

struct acrn_vcpu;

void vcpu_set_elr(struct acrn_vcpu *vcpu, uint64_t val);
void load_vcpu(struct acrn_vcpu *vcpu);
void unload_vcpu(struct acrn_vcpu *vcpu);
void flush_vcpu_context(struct acrn_vcpu *vcpu);
bool is_vcpu_context_updated(struct acrn_vcpu *vcpu);
void vcpu_mark_context_dirty(struct acrn_vcpu *vcpu);
uint64_t vcpu_get_vmpidr(struct acrn_vcpu *vcpu);
void arm64_run_vcpu(struct acrn_vcpu_arch *vcpu_arch);

#endif /* ARM64_GUEST_VCPU_PRIV_H */
