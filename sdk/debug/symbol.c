/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <sprintf.h>
#include <debug/symbol.h>

void dbg_resolve_symbol(uint64_t addr, struct dbg_symbol_desc *desc)
{
	uint32_t left = 0U;
	uint32_t right = dbg_symbol_count;
	uint32_t best = dbg_symbol_count;

	if (desc == NULL) {
		return;
	}

	desc->addr = addr;
	desc->offset = 0UL;
	desc->name = NULL;
	desc->found = false;

	if ((dbg_symbol_count == 0U) ||
		(addr < dbg_symbol_text_start) || (addr >= dbg_symbol_text_end)) {
		return;
	}

	while (left < right) {
		uint32_t mid = left + ((right - left) >> 1U);

		if (dbg_symbol_table[mid].addr <= addr) {
			best = mid;
			left = mid + 1U;
		} else {
			right = mid;
		}
	}

	if ((best != dbg_symbol_count) && (dbg_symbol_table[best].name != NULL)) {
		desc->offset = addr - dbg_symbol_table[best].addr;
		desc->name = dbg_symbol_table[best].name;
		desc->found = true;
	}
}

void dbg_format_symbol(uint64_t addr, char *buf, uint32_t size)
{
	struct dbg_symbol_desc desc;

	if ((buf == NULL) || (size == 0U)) {
		return;
	}

	dbg_resolve_symbol(addr, &desc);
	if (desc.found) {
		(void)snprintf(buf, size, "%s+0x%lx", desc.name, desc.offset);
	} else {
		(void)snprintf(buf, size, "unknown");
	}
}
