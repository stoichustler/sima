/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <bare.h>
#include <vm_config.h>
#include <asm/page.h>
#include <pgtable.h>

#include "linux_image_sizes.h"
#include "vm_config.h"

extern const uint8_t rk356x_lk_image_start[];
extern const uint8_t rk356x_lk_image_size[];
extern const uint8_t rk356x_zephyr_image_start[];
extern const uint8_t rk356x_zephyr_image_size[];

/*
 * rk356x follows the same static ARM64 memory contract as QEMU during bring-up:
 * each VM owns one configured host RAM window, and stage-2 maps that window
 * with guest IPA/GPA equal to host PA. The identity relationship is encoded by
 * setting arch.guest_ram_start and arch.guest_ram_hpa to the same platform
 * constant below. arch/arm64/guest/vm.c validates this before building the
 * stage-2 page tables, so hardware-platform changes must update both the VM
 * RAM window and the image load/entry addresses together.
 */
static struct vm_hpa_regions zephyr_memory_regions[] = {
	{
		.start_hpa = RK356X_ZEPHYR_RAM_START,
		.size_hpa = RK356X_ZEPHYR_RAM_SIZE,
	},
};

static struct vm_hpa_regions lk_memory_regions[] = {
	{
		.start_hpa = RK356X_LK_RAM_START,
		.size_hpa = RK356X_LK_RAM_SIZE,
	},
};

static struct vm_hpa_regions linux_memory_regions[] = {
	{
		.start_hpa = RK356X_LINUX_RAM_START,
		.size_hpa = RK356X_LINUX_RAM_SIZE,
	},
};

struct acrn_vm_config vm_configs[CONFIG_MAX_VM_NUM] = {
	[0] = {
		CONFIG_SERVICE_VM,
		.name = "zephyr",
		.cpu_affinity = AFFINITY_CPU(0),
		.guest_flags = GUEST_FLAG_STATIC_VM | GUEST_FLAG_NO_FW,
		.sched_params = {
			.bvt_weight = 128U,
		},
		.memory = {
			.size = RK356X_ZEPHYR_RAM_SIZE,
			.region_num = ARRAY_SIZE(zephyr_memory_regions),
			.host_regions = zephyr_memory_regions,
		},
		.os_config = {
			.name = "zephyr",
			.kernel_type = KERNEL_RAWIMAGE,
			.kernel_mod_tag = "zephyr",
			.kernel_load_addr = RK356X_ZEPHYR_RAM_START,
			.kernel_entry_addr = RK356X_ZEPHYR_RAM_START,
		},
		.arch = {
			.guest_ram_start = RK356X_ZEPHYR_RAM_START,
			.guest_ram_size = RK356X_ZEPHYR_RAM_SIZE,
			.guest_ram_hpa = RK356X_ZEPHYR_RAM_START,
			.guest_gicd_base = RK356X_GUEST_GICD_BASE,
			.guest_gicd_size = RK356X_GUEST_GICD_SIZE,
			.guest_gicr_base = RK356X_GUEST_GICR_BASE,
			.guest_gicr_size = RK356X_GUEST_GICR_SIZE,
			.guest_gicr_stride = RK356X_GUEST_GICR_STRIDE,
			.guest_uart_base = RK356X_GUEST_UART_BASE,
			.guest_uart_size = RK356X_GUEST_UART_SIZE,
			.guest_uart_irq = RK356X_GUEST_UART_IRQ,
		},
	},
	[1] = {
		CONFIG_PRE_STD_VM,
		.name = "lk",
		.cpu_affinity = AFFINITY_CPU(1),
		.guest_flags = GUEST_FLAG_STATIC_VM | GUEST_FLAG_NO_FW,
		.sched_params = {
			.bvt_weight = 16U,
		},
		.memory = {
			.size = RK356X_LK_RAM_SIZE,
			.region_num = ARRAY_SIZE(lk_memory_regions),
			.host_regions = lk_memory_regions,
		},
		.os_config = {
			.name = "lk",
			.kernel_type = KERNEL_RAWIMAGE,
			.kernel_mod_tag = "lk",
			.kernel_load_addr = RK356X_LK_RAM_START + 0x100000UL,
			.kernel_entry_addr = RK356X_LK_RAM_START + 0x100000UL,
		},
		.arch = {
			.guest_ram_start = RK356X_LK_RAM_START,
			.guest_ram_size = RK356X_LK_RAM_SIZE,
			.guest_ram_hpa = RK356X_LK_RAM_START,
			.guest_gicd_base = RK356X_GUEST_GICD_BASE,
			.guest_gicd_size = RK356X_GUEST_GICD_SIZE,
			.guest_gicr_base = RK356X_GUEST_GICR_BASE,
			.guest_gicr_size = RK356X_GUEST_GICR_SIZE,
			.guest_gicr_stride = RK356X_GUEST_GICR_STRIDE,
			.guest_uart_base = RK356X_GUEST_UART_BASE,
			.guest_uart_size = RK356X_GUEST_UART_SIZE,
			.guest_uart_irq = RK356X_GUEST_UART_IRQ,
		},
	},
	[2] = {
		CONFIG_PRE_STD_VM,
		.name = "linux",
		.cpu_affinity = AFFINITY_CPU(2),
		.guest_flags = GUEST_FLAG_STATIC_VM | GUEST_FLAG_NO_FW,
		.sched_params = {
			.bvt_weight = 64U,
		},
		.memory = {
			.size = RK356X_LINUX_RAM_SIZE,
			.region_num = ARRAY_SIZE(linux_memory_regions),
			.host_regions = linux_memory_regions,
		},
		.os_config = {
			.name = "linux",
			.kernel_type = KERNEL_RAWIMAGE,
			.kernel_mod_tag = "linux",
			.ramdisk_mod_tag = "Initramfs.cpio.gz",
			.bootargs = "console=ttyAMA0 rdinit=/init loglevel=7",
			.kernel_load_addr = RK356X_LINUX_KERNEL_LOAD_ADDR,
			.kernel_entry_addr = RK356X_LINUX_KERNEL_LOAD_ADDR,
			.kernel_ramdisk_addr = RK356X_LINUX_INITRAMFS_LOAD_ADDR,
			.kernel_ramdisk_size = BEAU_LINUX_INITRAMFS_SIZE,
		},
		.arch = {
			.guest_ram_start = RK356X_LINUX_RAM_START,
			.guest_ram_size = RK356X_LINUX_RAM_SIZE,
			.guest_ram_hpa = RK356X_LINUX_RAM_START,
			.guest_gicd_base = RK356X_GUEST_GICD_BASE,
			.guest_gicd_size = RK356X_GUEST_GICD_SIZE,
			.guest_gicr_base = RK356X_GUEST_GICR_BASE,
			.guest_gicr_size = RK356X_GUEST_GICR_SIZE,
			.guest_gicr_stride = RK356X_GUEST_GICR_STRIDE,
			.guest_uart_base = RK356X_GUEST_UART_BASE,
			.guest_uart_size = RK356X_GUEST_UART_SIZE,
			.guest_uart_irq = RK356X_GUEST_UART_IRQ,
		},
	},
};

struct acrn_vm_config *const service_vm_config = &vm_configs[0];

struct bare_boot_option bare_boot_options[] = {
	{
		.addr = (uint64_t)rk356x_zephyr_image_start,
		.size = (uint64_t)rk356x_zephyr_image_size,
		.tag = "zephyr",
	},
	{
		.addr = (uint64_t)rk356x_lk_image_start,
		.size = (uint64_t)rk356x_lk_image_size,
		.tag = "lk",
	},
	{
		.addr = RK356X_LINUX_IMAGE_STAGE_ADDR,
		.size = BEAU_LINUX_IMAGE_SIZE,
		.tag = "linux",
	},
	{
		.addr = RK356X_LINUX_INITRAMFS_STAGE_ADDR,
		.size = BEAU_LINUX_INITRAMFS_SIZE,
		.tag = "Initramfs.cpio.gz",
	},
};

uint16_t n_bare_boot_options = ARRAY_SIZE(bare_boot_options);
