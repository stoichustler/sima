/*
 * Copyright (C) 2026 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <pgtable.h>

uint64_t arch_pgtl_large(uint64_t pgtle)
{
	return ((pgtle & PAGE_DESC_VALID) != 0UL) && ((pgtle & PAGE_DESC_TYPE_MASK) == PAGE_BLOCK_DESC);
}

uint64_t arch_pgtl_page_paddr(uint64_t pgtle)
{
	return pgtle & PTE_PFN_MASK;
}

void *arch_hpa2hva_early(uint64_t x)
{
	return (void *)x;
}

uint64_t arch_hva2hpa_early(void *x)
{
	return (uint64_t)x;
}
