/*
 * Copyright (C) 2026 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <debug/symbol.h>

const struct dbg_symbol dbg_symbol_table[1] __attribute__((weak, used)) = {
	{ 0UL, NULL },
};
const uint32_t dbg_symbol_count __attribute__((weak, used)) = 0U;
const uint64_t dbg_symbol_text_start __attribute__((weak, used)) = 0UL;
const uint64_t dbg_symbol_text_end __attribute__((weak, used)) = 0UL;
