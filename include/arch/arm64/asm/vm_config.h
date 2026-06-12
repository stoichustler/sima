/*
 * Copyright (C) 2026 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_VM_CONFIG_H
#define ARM64_VM_CONFIG_H

#include <types.h>
#include <board_info.h>

#define MAX_VCPUS_PER_VM	MAX_PCPU_NUM
#define CONFIG_MAX_VM_NUM	16U

#define DM_OWNED_GUEST_FLAG_MASK	0UL

/*
 * ARM64 VM layout data is kept in the scenario configuration instead of being
 * hard-coded in generic virtualization code. guest_ram_* defines the stage-2
 * RAM IPA window. guest_ram_hpa is kept for platforms that need a non-identity
 * backing window, but the QEMU static RTOS layout intentionally keeps
 * IPA == PA. guest_gic* and guest_uart* define IPA ranges that trap to
 * vGIC/vPL011 MMIO handlers.
 */
struct arch_vm_config {
	uint64_t guest_ram_start;
	uint64_t guest_ram_size;
	uint64_t guest_ram_hpa;

	uint64_t guest_gicd_base;
	uint64_t guest_gicd_size;
	uint64_t guest_gicr_base;
	uint64_t guest_gicr_size;
	uint64_t guest_gicr_stride;

	uint64_t guest_uart_base;
	uint64_t guest_uart_size;
	uint32_t guest_uart_irq;
};

#endif /* ARM64_VM_CONFIG_H */
