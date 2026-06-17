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

extern const uint8_t qemu_lk_image_start[];
extern const uint8_t qemu_lk_image_size[];
extern const uint8_t qemu_zephyr_image_start[];
extern const uint8_t qemu_zephyr_image_size[];
extern const uint8_t qemu_sima_linux_dtb_start[];
extern const uint8_t qemu_sima_linux_dtb_size[];

/*
 * QEMU keeps a deliberately static VM layout for the SDK bring-up path:
 * - pCPU0..5 model ordinary cores; pCPU6..7 model performance cores.
 * - VM0 is the Zephyr service VM and is kept on ordinary cores only.
 * - VM1 is the LK pre-launched VM and may mix ordinary/performance cores.
 * - VM2 is a Linux raw-image VM and keeps OS-specific boot placement here.
 * - BSP vCPUs are placed on private pCPUs. Shared pCPUs are assigned to AP
 *   vCPUs so the baseline guests boot while still exercising scheduler sharing.
 *
 * Guest RAM windows are split and mapped with GPA/IPA == HPA. That keeps the
 * raw-image boot path simple and avoids firmware-discovered memory while
 * future VM2/VM3 entries can be added by extending this scenario table.
 */
static struct vm_hpa_regions zephyr_memory_regions[] = {
	{
		.start_hpa = QEMU_ZEPHYR_RAM_START,
		.size_hpa = QEMU_ZEPHYR_RAM_SIZE,
	},
};

static struct vm_hpa_regions lk_memory_regions[] = {
	{
		.start_hpa = QEMU_LK_RAM_START,
		.size_hpa = QEMU_LK_RAM_SIZE,
	},
};

static struct vm_hpa_regions linux_memory_regions[] = {
	{
		.start_hpa = QEMU_LINUX_RAM_START,
		.size_hpa = QEMU_LINUX_RAM_SIZE,
	},
};

struct acrn_vm_config vm_configs[CONFIG_MAX_VM_NUM] = {
	[0] = {
		CONFIG_SERVICE_VM,
		.name = "Zephyr",
		.cpu_affinity = AFFINITY_CPU(0) | AFFINITY_CPU(2) |
			AFFINITY_CPU(3) | AFFINITY_CPU(4),
		.guest_flags = GUEST_FLAG_STATIC_VM | GUEST_FLAG_NO_FW,
		.memory = {
			.size = QEMU_ZEPHYR_RAM_SIZE,
			.region_num = ARRAY_SIZE(zephyr_memory_regions),
			.host_regions = zephyr_memory_regions,
		},
		.os_config = {
			.name = "Zephyr",
			.kernel_type = KERNEL_RAWIMAGE,
			.kernel_mod_tag = "Zephyr",
			.kernel_load_addr = QEMU_ZEPHYR_RAM_START,
			.kernel_entry_addr = QEMU_ZEPHYR_RAM_START,
		},
		.arch = {
			.guest_ram_start = QEMU_ZEPHYR_RAM_START,
			.guest_ram_size = QEMU_ZEPHYR_RAM_SIZE,
			.guest_ram_hpa = QEMU_ZEPHYR_RAM_START,
			.guest_gicd_base = QEMU_GUEST_GICD_BASE,
			.guest_gicd_size = QEMU_GUEST_GICD_SIZE,
			.guest_gicr_base = QEMU_GUEST_GICR_BASE,
			.guest_gicr_size = QEMU_GUEST_GICR_SIZE,
			.guest_gicr_stride = QEMU_GUEST_GICR_STRIDE,
			.guest_uart_base = QEMU_GUEST_UART_BASE,
			.guest_uart_size = QEMU_GUEST_UART_SIZE,
			.guest_uart_irq = QEMU_GUEST_UART_IRQ,
		},
	},
	[1] = {
		CONFIG_PRE_STD_VM,
		.name = "LK",
		.cpu_affinity = AFFINITY_CPU(3) | AFFINITY_CPU(5) |
			AFFINITY_CPU(6) | AFFINITY_CPU(7),
		.guest_flags = GUEST_FLAG_STATIC_VM | GUEST_FLAG_NO_FW,
		.memory = {
			.size = QEMU_LK_RAM_SIZE,
			.region_num = ARRAY_SIZE(lk_memory_regions),
			.host_regions = lk_memory_regions,
		},
		.os_config = {
			.name = "LK",
			.kernel_type = KERNEL_RAWIMAGE,
			.kernel_mod_tag = "LK",
			.kernel_load_addr = 0x40100000UL,
			.kernel_entry_addr = 0x40100000UL,
		},
		.arch = {
			.guest_ram_start = QEMU_LK_RAM_START,
			.guest_ram_size = QEMU_LK_RAM_SIZE,
			.guest_ram_hpa = QEMU_LK_RAM_START,
			.guest_gicd_base = QEMU_GUEST_GICD_BASE,
			.guest_gicd_size = QEMU_GUEST_GICD_SIZE,
			.guest_gicr_base = QEMU_GUEST_GICR_BASE,
			.guest_gicr_size = QEMU_GUEST_GICR_SIZE,
			.guest_gicr_stride = QEMU_GUEST_GICR_STRIDE,
			.guest_uart_base = QEMU_GUEST_UART_BASE,
			.guest_uart_size = QEMU_GUEST_UART_SIZE,
			.guest_uart_irq = QEMU_GUEST_UART_IRQ,
		},
	},
	[2] = {
		CONFIG_PRE_STD_VM,
		.name = "Linux",
		.cpu_affinity = AFFINITY_CPU(1) | AFFINITY_CPU(4) |
			AFFINITY_CPU(6) | AFFINITY_CPU(7),
		.guest_flags = GUEST_FLAG_STATIC_VM,
		.memory = {
			.size = QEMU_LINUX_RAM_SIZE,
			.region_num = ARRAY_SIZE(linux_memory_regions),
			.host_regions = linux_memory_regions,
		},
		.os_config = {
			.name = "Linux",
			.kernel_type = KERNEL_RAWIMAGE,
			.kernel_mod_tag = "Linux",
			.ramdisk_mod_tag = "Initrd",
			.bootargs = "root=/dev/ram0 rw init=/linuxrc console=ttyAMA0 earlycon=pl011,0x09000000",
			.kernel_load_addr = QEMU_LINUX_KERNEL_LOAD_ADDR,
			.kernel_entry_addr = QEMU_LINUX_KERNEL_LOAD_ADDR,
			.kernel_ramdisk_addr = QEMU_LINUX_INITRD_LOAD_ADDR,
			.kernel_ramdisk_size = SIMA_LINUX_INITRD_SIZE,
		},
		.fdt_config = {
			.fdt_mod_tag = "Linux-dtb",
		},
		.arch = {
			.guest_ram_start = QEMU_LINUX_RAM_START,
			.guest_ram_size = QEMU_LINUX_RAM_SIZE,
			.guest_ram_hpa = QEMU_LINUX_RAM_START,
			.guest_gicd_base = QEMU_GUEST_GICD_BASE,
			.guest_gicd_size = QEMU_GUEST_GICD_SIZE,
			.guest_gicr_base = QEMU_GUEST_GICR_BASE,
			.guest_gicr_size = QEMU_GUEST_GICR_SIZE,
			.guest_gicr_stride = QEMU_GUEST_GICR_STRIDE,
			.guest_its_base = QEMU_GUEST_GITS_BASE,
			.guest_its_size = QEMU_GUEST_GITS_SIZE,
			.guest_uart_base = QEMU_GUEST_UART_BASE,
			.guest_uart_size = QEMU_GUEST_UART_SIZE,
			.guest_uart_irq = QEMU_GUEST_UART_IRQ,
		},
	},
};

struct acrn_vm_config *const service_vm_config = &vm_configs[0];

struct bare_boot_option bare_boot_options[] = {
	{
		.addr = (uint64_t)qemu_zephyr_image_start,
		.size = (uint64_t)qemu_zephyr_image_size,
		.tag = "Zephyr",
	},
	{
		.addr = (uint64_t)qemu_lk_image_start,
		.size = (uint64_t)qemu_lk_image_size,
		.tag = "LK",
	},
	{
		.addr = QEMU_LINUX_IMAGE_STAGE_ADDR,
		.size = SIMA_LINUX_IMAGE_SIZE,
		.tag = "Linux",
	},
	{
		.addr = QEMU_LINUX_INITRD_STAGE_ADDR,
		.size = SIMA_LINUX_INITRD_SIZE,
		.tag = "Initrd",
	},
	{
		.addr = (uint64_t)qemu_sima_linux_dtb_start,
		.size = (uint64_t)qemu_sima_linux_dtb_size,
		.tag = "Linux-dtb",
	},
};

uint16_t n_bare_boot_options = ARRAY_SIZE(bare_boot_options);
