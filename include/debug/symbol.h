/*
 * Copyright (C) 2026 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef SYMBOL_H
#define SYMBOL_H

#include <types.h>

struct dbg_symbol {
	uint64_t addr;
	const char *name;
};

struct dbg_symbol_desc {
	uint64_t addr;
	uint64_t offset;
	const char *name;
	bool found;
};

extern const struct dbg_symbol dbg_symbol_table[];
extern const uint32_t dbg_symbol_count;
extern const uint64_t dbg_symbol_text_start;
extern const uint64_t dbg_symbol_text_end;

void dbg_resolve_symbol(uint64_t addr, struct dbg_symbol_desc *desc);
void dbg_format_symbol(uint64_t addr, char *buf, uint32_t size);

#endif /* SYMBOL_H */
