/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_INIT_H
#define ARM64_INIT_H

#include <types.h>

void init_primary_pcpu(uint64_t mpidr, uint64_t fdt_paddr);
void init_secondary_pcpu(uint64_t mpidr);

#endif /* ARM64_INIT_H */
