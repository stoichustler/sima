/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_MM_COMMON_H
#define ARM64_MM_COMMON_H

#include <asm/pgtable.h>

#define PGTL3_SHIFT		PGD_SHIFT
#define PGTL3_SIZE		(1UL << PGTL3_SHIFT)
#define PGTL3_MASK		(~(PGTL3_SIZE - 1UL))
#define PTRS_PER_PGTL3E		PTRS_PER_PGD
#define PGTL2_SHIFT		PUD_SHIFT
#define PGTL2_SIZE		(1UL << PGTL2_SHIFT)
#define PGTL2_MASK		(~(PGTL2_SIZE - 1UL))
#define PTRS_PER_PGTL2E		PTRS_PER_PUD
#define PGTL1_SHIFT		PMD_SHIFT
#define PGTL1_SIZE		(1UL << PGTL1_SHIFT)
#define PGTL1_MASK		(~(PGTL1_SIZE - 1UL))
#define PTRS_PER_PGTL1E		PTRS_PER_PMD
#define PGTL0_SHIFT		PTE_SHIFT
#define PGTL0_SIZE		(1UL << PGTL0_SHIFT)
#define PGTL0_MASK		(~(PGTL0_SIZE - 1UL))
#define PTRS_PER_PGTL0E		PTRS_PER_PTE

#endif /* ARM64_MM_COMMON_H */
