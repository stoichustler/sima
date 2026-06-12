/*
 * Copyright (C) 2026 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <vm_config.h>
#include <asm/platform.h>
#include <logmsg.h>

#define QEMU_VIRT_RAM_START		0x40000000UL
#define QEMU_VIRT_RAM_SIZE		0x40000000UL

#define QEMU_VIRT_LOW_MMIO_START	0x00000000UL
#define QEMU_VIRT_LOW_MMIO_SIZE		0x10000000UL
#define QEMU_VIRT_PL011_BASE		0x09000000UL
#define QEMU_VIRT_GICD_BASE		0x08000000UL
#define QEMU_VIRT_GICD_SIZE		0x00010000UL
#define QEMU_VIRT_GICR_BASE		0x080A0000UL
#define QEMU_VIRT_GICR_STRIDE		0x00020000UL
#define QEMU_VIRT_GICR_SIZE		(MAX_PCPU_NUM * QEMU_VIRT_GICR_STRIDE)
#define QEMU_VIRT_GIC_MMIO_START	QEMU_VIRT_GICD_BASE
#define QEMU_VIRT_GIC_MMIO_END		QEMU_VIRT_PL011_BASE
#define QEMU_VIRT_GIC_IIDR		0x43bU

#define QEMU_VIRT_PCIE_MMIO_START	0x10000000UL
#define QEMU_VIRT_PCIE_MMIO_SIZE	0x30000000UL

static const struct arm64_mem_region platform_mmio_regions[] = {
	{
		.base = QEMU_VIRT_LOW_MMIO_START,
		.size = QEMU_VIRT_LOW_MMIO_SIZE,
	},
	{
		.base = QEMU_VIRT_PCIE_MMIO_START,
		.size = QEMU_VIRT_PCIE_MMIO_SIZE,
	},
};

static const struct arch_vm_config *qemu_guest_config(uint16_t vm_id)
{
	const struct acrn_vm_config *vm_config;

	if (vm_id >= CONFIG_MAX_VM_NUM) {
		panic("qemu vm%hu is out of range", vm_id);
	}

	vm_config = get_vm_config(vm_id);
	if ((vm_config->cpu_affinity == 0UL) || (vm_config->arch.guest_ram_size == 0UL)) {
		panic("qemu vm%hu has no guest platform config", vm_id);
	}

	return &vm_config->arch;
}

const struct arm64_mem_region *arm64_get_platform_mmio_regions(uint32_t *count)
{
	*count = ARRAY_SIZE(platform_mmio_regions);
	return platform_mmio_regions;
}

uint64_t arm64_platform_ram_start(void)
{
	return QEMU_VIRT_RAM_START;
}

uint64_t arm64_platform_ram_size(void)
{
	return QEMU_VIRT_RAM_SIZE;
}

uint64_t arm64_platform_console_mmio_base(void)
{
	return QEMU_VIRT_PL011_BASE;
}

uint64_t arm64_platform_guest_ram_start(uint16_t vm_id)
{
	return qemu_guest_config(vm_id)->guest_ram_start;
}

uint64_t arm64_platform_guest_ram_size(uint16_t vm_id)
{
	return qemu_guest_config(vm_id)->guest_ram_size;
}

uint64_t arm64_platform_guest_ram_hpa(uint16_t vm_id)
{
	return qemu_guest_config(vm_id)->guest_ram_hpa;
}

uint64_t arm64_platform_guest_gicd_base(uint16_t vm_id)
{
	return qemu_guest_config(vm_id)->guest_gicd_base;
}

uint64_t arm64_platform_guest_gicd_size(uint16_t vm_id)
{
	return qemu_guest_config(vm_id)->guest_gicd_size;
}

uint64_t arm64_platform_guest_gicr_base(uint16_t vm_id)
{
	return qemu_guest_config(vm_id)->guest_gicr_base;
}

uint64_t arm64_platform_guest_gicr_stride(uint16_t vm_id)
{
	return qemu_guest_config(vm_id)->guest_gicr_stride;
}

uint64_t arm64_platform_guest_gicr_size(uint16_t vm_id)
{
	return qemu_guest_config(vm_id)->guest_gicr_size;
}

uint64_t arm64_platform_guest_uart_base(uint16_t vm_id)
{
	return qemu_guest_config(vm_id)->guest_uart_base;
}

uint64_t arm64_platform_guest_uart_size(uint16_t vm_id)
{
	return qemu_guest_config(vm_id)->guest_uart_size;
}

uint32_t arm64_platform_guest_uart_irq(uint16_t vm_id)
{
	return qemu_guest_config(vm_id)->guest_uart_irq;
}

uint64_t arm64_platform_gicd_base(void)
{
	return QEMU_VIRT_GICD_BASE;
}

uint64_t arm64_platform_gicd_size(void)
{
	return QEMU_VIRT_GICD_SIZE;
}

uint64_t arm64_platform_gicr_base(void)
{
	return QEMU_VIRT_GICR_BASE;
}

uint64_t arm64_platform_gicr_stride(void)
{
	return QEMU_VIRT_GICR_STRIDE;
}

uint64_t arm64_platform_gicr_size(void)
{
	return QEMU_VIRT_GICR_SIZE;
}

uint64_t arm64_platform_gic_mmio_start(void)
{
	return QEMU_VIRT_GIC_MMIO_START;
}

uint64_t arm64_platform_gic_mmio_end(void)
{
	return QEMU_VIRT_GIC_MMIO_END;
}

uint32_t arm64_platform_gic_iidr(void)
{
	return QEMU_VIRT_GIC_IIDR;
}
