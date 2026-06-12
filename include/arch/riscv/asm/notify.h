/*
 * Copyright (C) 2023-2025 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Authors:
 *   Haicheng Li <haicheng.li@intel.com>
 */

#ifndef RISCV_NOTIFY_H
#define RISCV_NOTIFY_H

#include <types.h>

void arch_init_smp_call(void);
void arch_smp_call_kick_pcpu(uint16_t pcpu_id);

#endif /* RISCV_NOTIFY_H */
