/*
 * Copyright (C) 2026 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_PER_CPU_H
#define ARM64_PER_CPU_H

#include <types.h>

struct per_cpu_arch {
	uint64_t mpidr;
};

#endif /* ARM64_PER_CPU_H */
