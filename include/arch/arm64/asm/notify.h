/*
 * Copyright (C) 2026 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_NOTIFY_H
#define ARM64_NOTIFY_H

#include <types.h>

void arch_init_smp_call(void);
void arch_smp_call_kick_pcpu(uint16_t pcpu_id);

#endif /* ARM64_NOTIFY_H */
