/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_PLATFORM_H
#define ARM64_PLATFORM_H

#include <types.h>

#ifndef ASSEMBLER
struct arm64_mem_region {
	uint64_t base;
	uint64_t size;
};

const struct arm64_mem_region *arm64_get_platform_mmio_regions(uint32_t *count);
uint64_t arm64_platform_ram_start(void);
uint64_t arm64_platform_ram_size(void);
uint64_t arm64_platform_console_mmio_base(void);
uint64_t arm64_platform_gicd_base(void);
uint64_t arm64_platform_gicd_size(void);
uint64_t arm64_platform_gicr_base(void);
uint64_t arm64_platform_gicr_stride(void);
uint64_t arm64_platform_gicr_size(void);
uint64_t arm64_platform_gits_base(void);
uint64_t arm64_platform_gits_size(void);
uint64_t arm64_platform_gic_mmio_start(void);
uint64_t arm64_platform_gic_mmio_end(void);
uint32_t arm64_platform_gic_iidr(void);
#endif

#endif /* ARM64_PLATFORM_H */
