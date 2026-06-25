/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef VM_WDT_H
#define VM_WDT_H

#include <types.h>

#ifndef CONFIG_VM_WDT_MONITOR_VM_NUM
#define CONFIG_VM_WDT_MONITOR_VM_NUM	3U
#endif

#ifndef CONFIG_VM_WDT_PRINT_PERIOD_MS
#define CONFIG_VM_WDT_PRINT_PERIOD_MS	5000U
#endif

#ifndef CONFIG_VM_WDT_TIMEOUT_MS
#define CONFIG_VM_WDT_TIMEOUT_MS	15000U
#endif

#ifndef CONFIG_VM_WDT_ACTIVITY_SAMPLE_MS
#define CONFIG_VM_WDT_ACTIVITY_SAMPLE_MS	1000U
#endif

struct acrn_vm;

enum vm_wdt_status {
	VM_WDT_STATUS_UNUSED = 0U,
	VM_WDT_STATUS_OFFLINE,
	VM_WDT_STATUS_UNKNOWN,
	VM_WDT_STATUS_ALIVE,
	VM_WDT_STATUS_STUCK,
};

struct vm_wdt_snapshot {
	enum vm_wdt_status status;
	uint64_t age_ms;
	uint64_t kick_count;
	uint64_t last_token;
};

void vm_wdt_start(void);
void vm_wdt_reset(const struct acrn_vm *vm);
void vm_wdt_activity(const struct acrn_vm *vm);
void vm_wdt_kick(const struct acrn_vm *vm, uint64_t token);
int32_t vm_wdt_get_snapshot(uint16_t vm_id, struct vm_wdt_snapshot *snapshot);

#endif /* VM_WDT_H */
