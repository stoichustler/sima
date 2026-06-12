/*
 * Copyright (C) 2026 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_GUEST_VCPU_EXIT_H
#define ARM64_GUEST_VCPU_EXIT_H

#ifndef ASSEMBLER
struct cpu_regs;

void dispatch_vcpu_trap(struct cpu_regs *regs);
void dispatch_vcpu_irq(struct cpu_regs *regs);
#endif

#endif /* ARM64_GUEST_VCPU_EXIT_H */
