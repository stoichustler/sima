/*
 * Copyright (C) 2026 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_PAGE_H
#define ARM64_PAGE_H

#include <types.h>

#define PAGE_SHIFT	12U
#define PAGE_SIZE	(1UL << PAGE_SHIFT)
#define PAGE_MASK	0xFFFFFFFFFFFFF000UL

#endif /* ARM64_PAGE_H */
