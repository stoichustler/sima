/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <vm.h>
#include <vm_config.h>
#include <vfdt.h>
#include <fdt_api.h>
#include <libfdt.h>
#include <logmsg.h>
#include <sprintf.h>
#include <asm/platform.h>

#include "platform_rk356x.h"
#include "vm_config.h"

#define RK356X_FDT_PHANDLE_GIC		1U
#define RK356X_FDT_PHANDLE_RAM		2U
#define RK356X_FDT_PHANDLE_UART		3U
#define RK356X_FDT_PHANDLE_UARTCLK	4U

#define RK356X_FDT_GIC_SPI		0U
#define RK356X_FDT_GIC_PPI		1U
#define RK356X_FDT_IRQ_TYPE_EDGE	1U
#define RK356X_FDT_IRQ_TYPE_LEVEL	4U

#define RK356X_FDT_UART_CLOCK_HZ	24000000U
#define RK356X_FDT_UART_BAUD		115200U

static const struct arm64_mem_region platform_mmio_regions[] = {
	{
		.base = RK356X_MMIO_START,
		.size = RK356X_MMIO_SIZE,
	},
};

const struct arm64_mem_region *arm64_get_platform_mmio_regions(uint32_t *count)
{
	*count = ARRAY_SIZE(platform_mmio_regions);
	return platform_mmio_regions;
}

uint64_t arm64_platform_ram_start(void)
{
	return RK356X_RAM_START;
}

uint64_t arm64_platform_ram_size(void)
{
	return RK356X_RAM_SIZE;
}

uint64_t arm64_platform_console_mmio_base(void)
{
	return RK356X_UART0_BASE;
}

uint64_t arm64_platform_gicd_base(void)
{
	return RK356X_GICD_BASE;
}

uint64_t arm64_platform_gicd_size(void)
{
	return RK356X_GICD_SIZE;
}

uint64_t arm64_platform_gicr_base(void)
{
	return RK356X_GICR_BASE;
}

uint64_t arm64_platform_gicr_stride(void)
{
	return RK356X_GICR_STRIDE;
}

uint64_t arm64_platform_gicr_size(void)
{
	return RK356X_GICR_SIZE;
}

uint64_t arm64_platform_gits_base(void)
{
	return 0UL;
}

uint64_t arm64_platform_gits_size(void)
{
	return 0UL;
}

uint64_t arm64_platform_gic_mmio_start(void)
{
	return RK356X_GIC_MMIO_START;
}

uint64_t arm64_platform_gic_mmio_end(void)
{
	return RK356X_GIC_MMIO_END;
}

uint32_t arm64_platform_gic_iidr(void)
{
	return RK356X_GIC_IIDR;
}

static void fdt_check_ret(int32_t ret, const char *op)
{
	if (ret < 0) {
		panic("failed to build rk356x service vm fdt: %s ret=%d", op, ret);
	}
}

static void fdt_property_reg64(void *fdt, const char *name, uint64_t addr, uint64_t size)
{
	fdt32_t reg[4];

	reg[0] = cpu_to_fdt32((uint32_t)(addr >> 32U));
	reg[1] = cpu_to_fdt32((uint32_t)addr);
	reg[2] = cpu_to_fdt32((uint32_t)(size >> 32U));
	reg[3] = cpu_to_fdt32((uint32_t)size);
	fdt_check_ret(fdt_property(fdt, name, reg, sizeof(reg)), name);
}

static void fdt_property_gic_reg(void *fdt, struct acrn_vm *vm)
{
	fdt32_t reg[8];
	const struct arch_vm_config *arch_config = &get_vm_config(vm->vm_id)->arch;
	uint64_t gicd_base = arch_config->guest_gicd_base;
	uint64_t gicd_size = arch_config->guest_gicd_size;
	uint64_t gicr_base = arch_config->guest_gicr_base;
	uint64_t gicr_size = arch_config->guest_gicr_size;

	reg[0] = cpu_to_fdt32((uint32_t)(gicd_base >> 32U));
	reg[1] = cpu_to_fdt32((uint32_t)gicd_base);
	reg[2] = cpu_to_fdt32((uint32_t)(gicd_size >> 32U));
	reg[3] = cpu_to_fdt32((uint32_t)gicd_size);
	reg[4] = cpu_to_fdt32((uint32_t)(gicr_base >> 32U));
	reg[5] = cpu_to_fdt32((uint32_t)gicr_base);
	reg[6] = cpu_to_fdt32((uint32_t)(gicr_size >> 32U));
	reg[7] = cpu_to_fdt32((uint32_t)gicr_size);
	fdt_check_ret(fdt_property(fdt, "reg", reg, sizeof(reg)), "gic reg");
}

static void fdt_property_irq4(void *fdt, const char *name, const uint32_t *cells,
	uint32_t nr_cells)
{
	fdt32_t values[12];
	uint32_t i;

	for (i = 0U; i < nr_cells; i++) {
		values[i] = cpu_to_fdt32(cells[i]);
	}

	fdt_check_ret(fdt_property(fdt, name, values, (int32_t)(nr_cells * sizeof(fdt32_t))), name);
}

static void fdt_begin_cpu_node(void *fdt, uint32_t vcpu_id)
{
	char name[16];
	fdt32_t reg = cpu_to_fdt32(vcpu_id);

	snprintf(name, sizeof(name), "cpu@%u", vcpu_id);
	fdt_check_ret(fdt_begin_node(fdt, name), name);
	fdt_check_ret(fdt_property_string(fdt, "device_type", "cpu"), "cpu device_type");
	fdt_check_ret(fdt_property_string(fdt, "compatible", "arm,cortex-a55"), "cpu compatible");
	fdt_check_ret(fdt_property_string(fdt, "enable-method", "psci"), "cpu enable-method");
	fdt_check_ret(fdt_property(fdt, "reg", &reg, sizeof(reg)), "cpu reg");
	fdt_check_ret(fdt_end_node(fdt), "cpu end");
}

static void fdt_add_chosen(void *fdt, struct acrn_vm *vm)
{
	const struct acrn_vm_config *vm_config = get_vm_config(vm->vm_id);
	const struct arch_vm_config *arch_config = &vm_config->arch;
	uint64_t initrd_start = vm_config->os_config.kernel_ramdisk_addr;
	uint64_t initrd_size = vm_config->os_config.kernel_ramdisk_size;
	char stdout_path[32];

	snprintf(stdout_path, sizeof(stdout_path), "/serial@%lx", arch_config->guest_uart_base);
	fdt_check_ret(fdt_begin_node(fdt, "chosen"), "chosen");
	fdt_check_ret(fdt_property_string(fdt, "stdout-path", stdout_path), "stdout-path");
	if ((initrd_start != 0UL) && (initrd_size != 0UL)) {
		fdt_check_ret(fdt_property_u64(fdt, "linux,initrd-start", initrd_start),
			"initrd-start");
		fdt_check_ret(fdt_property_u64(fdt, "linux,initrd-end", initrd_start + initrd_size),
			"initrd-end");
	}
	fdt_check_ret(fdt_end_node(fdt), "chosen end");
}

static void fdt_add_cpus(void *fdt, const struct acrn_vm *vm)
{
	uint64_t cpu_bitmap = get_vm_config(vm->vm_id)->cpu_affinity;
	uint32_t cpu_count = 0U;
	uint32_t i;

	fdt_check_ret(fdt_begin_node(fdt, "cpus"), "cpus");
	fdt_check_ret(fdt_property_u32(fdt, "#address-cells", 1U), "cpus address-cells");
	fdt_check_ret(fdt_property_u32(fdt, "#size-cells", 0U), "cpus size-cells");

	for (i = 0U; i < MAX_PCPU_NUM; i++) {
		if ((cpu_bitmap & (1UL << i)) != 0UL) {
			fdt_begin_cpu_node(fdt, cpu_count);
			cpu_count++;
		}
	}

	fdt_check_ret(fdt_end_node(fdt), "cpus end");
}

static void fdt_add_memory(void *fdt, struct acrn_vm *vm)
{
	const struct arch_vm_config *arch_config = &get_vm_config(vm->vm_id)->arch;
	uint64_t ram_start = arch_config->guest_ram_start;
	uint64_t ram_size = arch_config->guest_ram_size;

	fdt_check_ret(fdt_begin_node(fdt, "memory@0"), "memory");
	fdt_check_ret(fdt_property_string(fdt, "device_type", "memory"), "memory device_type");
	fdt_property_reg64(fdt, "reg", ram_start, ram_size);
	fdt_check_ret(fdt_property_u32(fdt, "phandle", RK356X_FDT_PHANDLE_RAM), "memory phandle");
	fdt_check_ret(fdt_end_node(fdt), "memory end");
}

static void fdt_add_psci(void *fdt)
{
	fdt_check_ret(fdt_begin_node(fdt, "psci"), "psci");
	fdt_check_ret(fdt_property_string(fdt, "compatible", "arm,psci-1.0"), "psci compatible");
	fdt_check_ret(fdt_property_string(fdt, "method", "hvc"), "psci method");
	fdt_check_ret(fdt_end_node(fdt), "psci end");
}

static void fdt_add_gic(void *fdt, struct acrn_vm *vm)
{
	const struct arch_vm_config *arch_config = &get_vm_config(vm->vm_id)->arch;
	uint64_t gicd_base = arch_config->guest_gicd_base;
	char name[32];

	snprintf(name, sizeof(name), "interrupt-controller@%lx", gicd_base);
	fdt_check_ret(fdt_begin_node(fdt, name), "gic");
	fdt_check_ret(fdt_property_string(fdt, "compatible", "arm,gic-v3"), "gic compatible");
	fdt_check_ret(fdt_property(fdt, "interrupt-controller", NULL, 0), "gic interrupt-controller");
	fdt_check_ret(fdt_property_u32(fdt, "#interrupt-cells", 3U), "gic interrupt-cells");
	fdt_check_ret(fdt_property_u32(fdt, "#address-cells", 2U), "gic address-cells");
	fdt_check_ret(fdt_property_u32(fdt, "#size-cells", 2U), "gic size-cells");
	fdt_property_gic_reg(fdt, vm);
	fdt_check_ret(fdt_property_u32(fdt, "phandle", RK356X_FDT_PHANDLE_GIC), "gic phandle");
	fdt_check_ret(fdt_end_node(fdt), "gic end");
}

static void fdt_add_timer(void *fdt)
{
	const uint32_t interrupts[] = {
		RK356X_FDT_GIC_PPI, 13U, RK356X_FDT_IRQ_TYPE_LEVEL,
		RK356X_FDT_GIC_PPI, 14U, RK356X_FDT_IRQ_TYPE_LEVEL,
		RK356X_FDT_GIC_PPI, 11U, RK356X_FDT_IRQ_TYPE_LEVEL,
		RK356X_FDT_GIC_PPI, 10U, RK356X_FDT_IRQ_TYPE_LEVEL,
	};

	fdt_check_ret(fdt_begin_node(fdt, "timer"), "timer");
	fdt_check_ret(fdt_property_string(fdt, "compatible", "arm,armv8-timer"), "timer compatible");
	fdt_property_irq4(fdt, "interrupts", interrupts, ARRAY_SIZE(interrupts));
	fdt_check_ret(fdt_end_node(fdt), "timer end");
}

static void fdt_add_uart_clock(void *fdt)
{
	fdt_check_ret(fdt_begin_node(fdt, "uart-clock"), "uart-clock");
	fdt_check_ret(fdt_property_string(fdt, "compatible", "fixed-clock"), "clock compatible");
	fdt_check_ret(fdt_property_u32(fdt, "#clock-cells", 0U), "clock cells");
	fdt_check_ret(fdt_property_u32(fdt, "clock-frequency", RK356X_FDT_UART_CLOCK_HZ), "clock freq");
	fdt_check_ret(fdt_property_u32(fdt, "phandle", RK356X_FDT_PHANDLE_UARTCLK), "clock phandle");
	fdt_check_ret(fdt_end_node(fdt), "clock end");
}

static void fdt_add_uart(void *fdt, struct acrn_vm *vm)
{
	const struct arch_vm_config *arch_config = &get_vm_config(vm->vm_id)->arch;
	uint64_t uart_base = arch_config->guest_uart_base;
	uint32_t irq = arch_config->guest_uart_irq;
	const uint32_t interrupts[] = {
		RK356X_FDT_GIC_SPI, irq - 32U, RK356X_FDT_IRQ_TYPE_LEVEL,
	};
	fdt32_t clock = cpu_to_fdt32(RK356X_FDT_PHANDLE_UARTCLK);
	char name[32];

	snprintf(name, sizeof(name), "serial@%lx", uart_base);
	fdt_check_ret(fdt_begin_node(fdt, name), "uart");
	fdt_check_ret(fdt_property_string(fdt, "compatible", "arm,pl011"), "uart compatible");
	fdt_property_reg64(fdt, "reg", uart_base, arch_config->guest_uart_size);
	fdt_property_irq4(fdt, "interrupts", interrupts, ARRAY_SIZE(interrupts));
	fdt_check_ret(fdt_property(fdt, "clocks", &clock, sizeof(clock)), "uart clocks");
	fdt_check_ret(fdt_property_u32(fdt, "clock-frequency", RK356X_FDT_UART_CLOCK_HZ), "uart clock-frequency");
	fdt_check_ret(fdt_property_u32(fdt, "current-speed", RK356X_FDT_UART_BAUD), "uart current-speed");
	fdt_check_ret(fdt_property_u32(fdt, "phandle", RK356X_FDT_PHANDLE_UART), "uart phandle");
	fdt_check_ret(fdt_end_node(fdt), "uart end");
}

void arch_init_service_vm_vfdt(struct acrn_vm *vm)
{
	void *fdt = vm_get_vfdt(vm);

	fdt_check_ret(fdt_create(fdt, MAX_FDT_SIZE), "create");
	fdt_check_ret(fdt_finish_reservemap(fdt), "reservemap");
	fdt_check_ret(fdt_begin_node(fdt, ""), "root");
	fdt_check_ret(fdt_property_u32(fdt, "#address-cells", 2U), "root address-cells");
	fdt_check_ret(fdt_property_u32(fdt, "#size-cells", 2U), "root size-cells");
	fdt_check_ret(fdt_property_string(fdt, "compatible", "rockchip,rk3568"), "root compatible");
	fdt_add_chosen(fdt, vm);
	fdt_add_cpus(fdt, vm);
	fdt_add_memory(fdt, vm);
	fdt_add_psci(fdt);
	fdt_add_gic(fdt, vm);
	fdt_add_timer(fdt);
	fdt_add_uart_clock(fdt);
	fdt_add_uart(fdt, vm);
	fdt_check_ret(fdt_end_node(fdt), "root end");
	fdt_check_ret(fdt_finish(fdt), "finish");

	pr_info("vm-%hu static vfdt size=0x%x", vm->vm_id, fdt_totalsize(fdt));
}
