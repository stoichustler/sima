/*
 * Copyright (C) 2018-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <pci.h>
#include <console.h>

size_t console_write(__unused const char *str, __unused size_t len)
{
	return 0;
}

char console_getc(void)
{
	return '\0';
}

void console_putc(__unused const char *ch) {}

void console_init(void) {}
void console_setup_timer(void) {}

void suspend_console(void) {}
void resume_console(void) {}

bool console_vm_tx_put(__unused uint16_t vmid, __unused char ch) { return false; }
void console_vm_ring_drain(__unused uint16_t vmid) {}
void console_vm_exception_log(__unused uint16_t vmid, __unused const char *buf, __unused size_t len) {}
uint32_t console_vm_exception_count(void) { return 0U; }
bool console_vm_exception_get_stats(__unused uint16_t vmid, __unused struct console_vm_exception_stats *stats)
{
	return false;
}
uint32_t console_vm_exception_copy(__unused uint16_t vmid, __unused uint32_t offset,
	__unused char *buf, __unused uint32_t len)
{
	return 0U;
}
void console_vm_exception_clear(__unused uint16_t vmid) {}
void console_vm_exception_clear_all(void) {}

bool handle_dbg_cmd(__unused const char *cmd, __unused int32_t len) { return false; }
void console_vmexit_callback(__unused struct acrn_vcpu *vcpu) {}

void shell_init(void) {}
void shell_kick(void) {}

bool console_need_log(__unused uint32_t severity) { return false; }
void console_log(__unused char *buffer) {}
