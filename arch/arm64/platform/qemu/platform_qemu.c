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

#include "platform_qemu.h"
#include "vm_config.h"

#define QEMU_FDT_PHANDLE_GIC		1U
#define QEMU_FDT_PHANDLE_RAM		2U
#define QEMU_FDT_PHANDLE_UART		3U
#define QEMU_FDT_PHANDLE_UARTCLK	4U
#define QEMU_FDT_PHANDLE_ITS		5U

#define QEMU_FDT_GIC_SPI		0U
#define QEMU_FDT_GIC_PPI		1U
#define QEMU_FDT_IRQ_TYPE_EDGE		1U
#define QEMU_FDT_IRQ_TYPE_LEVEL		4U

#define QEMU_FDT_UART_CLOCK_HZ		24000000U
#define QEMU_FDT_UART_BAUD		115200U

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

const struct beau_config beau_config = {
	.ram_start = QEMU_VIRT_RAM_START,
	.ram_size = QEMU_VIRT_RAM_SIZE,
	.console_mmio_base = QEMU_VIRT_PL011_BASE,
	.gicd_base = QEMU_VIRT_GICD_BASE,
	.gicd_size = QEMU_VIRT_GICD_SIZE,
	.gicr_base = QEMU_VIRT_GICR_BASE,
	.gicr_stride = QEMU_VIRT_GICR_STRIDE,
	.gicr_size = QEMU_VIRT_GICR_SIZE,
	.gits_base = QEMU_VIRT_GITS_BASE,
	.gits_size = QEMU_VIRT_GITS_SIZE,
	.gic_iidr = QEMU_VIRT_GIC_IIDR,
};

const struct arm64_mem_region *arm64_get_platform_mmio_regions(uint32_t *count)
{
	*count = ARRAY_SIZE(platform_mmio_regions);
	return platform_mmio_regions;
}

static void fdt_check_ret(int32_t ret, const char *op)
{
	if (ret < 0) {
		panic("failed to build qemu service vm fdt: %s ret=%d", op, ret);
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
	fdt_check_ret(fdt_property_string(fdt, "compatible", "arm,cortex-a57"), "cpu compatible");
	fdt_check_ret(fdt_property_string(fdt, "enable-method", "psci"), "cpu enable-method");
	fdt_check_ret(fdt_property(fdt, "reg", &reg, sizeof(reg)), "cpu reg");
	fdt_check_ret(fdt_end_node(fdt), "cpu end");
}

static void fdt_add_chosen(void *fdt, struct acrn_vm *vm)
{
	const struct arch_vm_config *arch_config = &get_vm_config(vm->vm_id)->arch;
	char stdout_path[32];

	snprintf(stdout_path, sizeof(stdout_path), "/pl011@%lx", arch_config->guest_uart_base);
	fdt_check_ret(fdt_begin_node(fdt, "chosen"), "chosen");
	fdt_check_ret(fdt_property_string(fdt, "stdout-path", stdout_path), "stdout-path");
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

	while (cpu_bitmap != 0UL) {
		cpu_bitmap &= cpu_bitmap - 1UL;
		cpu_count++;
	}

	for (i = 0U; i < cpu_count; i++) {
		fdt_begin_cpu_node(fdt, i);
	}

	fdt_check_ret(fdt_end_node(fdt), "cpus end");
}

static void fdt_add_psci(void *fdt)
{
	static const char psci_compat[] = "arm,psci-1.0\0arm,psci-0.2\0arm,psci";

	fdt_check_ret(fdt_begin_node(fdt, "psci"), "psci");
	fdt_check_ret(fdt_property(fdt, "compatible", psci_compat, sizeof(psci_compat)),
		"psci compatible");
	fdt_check_ret(fdt_property_string(fdt, "method", "hvc"), "psci method");
	fdt_check_ret(fdt_end_node(fdt), "psci end");
}

static void fdt_add_memory(void *fdt, struct acrn_vm *vm)
{
	const struct arch_vm_config *arch_config = &get_vm_config(vm->vm_id)->arch;
	char name[32];
	uint64_t ram_start = arch_config->guest_ram_start;
	uint64_t ram_size = arch_config->guest_ram_size;

	snprintf(name, sizeof(name), "memory@%lx", ram_start);
	fdt_check_ret(fdt_begin_node(fdt, name), "memory");
	fdt_check_ret(fdt_property_string(fdt, "device_type", "memory"), "memory device_type");
	fdt_check_ret(fdt_property_u32(fdt, "phandle", QEMU_FDT_PHANDLE_RAM), "memory phandle");
	fdt_property_reg64(fdt, "reg", ram_start, ram_size);
	fdt_check_ret(fdt_end_node(fdt), "memory end");
}

static void fdt_add_gic(void *fdt, struct acrn_vm *vm)
{
	const struct arch_vm_config *arch_config = &get_vm_config(vm->vm_id)->arch;
	char name[48];
	uint64_t gicd_base = arch_config->guest_gicd_base;

	snprintf(name, sizeof(name), "interrupt-controller@%lx", gicd_base);
	fdt_check_ret(fdt_begin_node(fdt, name), "gic");
	fdt_check_ret(fdt_property_string(fdt, "compatible", "arm,gic-v3"), "gic compatible");
	fdt_check_ret(fdt_property_u32(fdt, "#interrupt-cells", 3U), "gic interrupt-cells");
	fdt_check_ret(fdt_property(fdt, "interrupt-controller", NULL, 0), "gic controller");
	fdt_check_ret(fdt_property_u32(fdt, "phandle", QEMU_FDT_PHANDLE_GIC), "gic phandle");
	fdt_property_gic_reg(fdt, vm);
	fdt_check_ret(fdt_end_node(fdt), "gic end");
}

static void fdt_add_its(void *fdt, struct acrn_vm *vm)
{
	const struct arch_vm_config *arch_config = &get_vm_config(vm->vm_id)->arch;
	char name[48];
	uint64_t its_base = arch_config->guest_its_base;
	uint64_t its_size = arch_config->guest_its_size;

	if (its_size == 0UL) {
		return;
	}

	snprintf(name, sizeof(name), "msi-controller@%lx", its_base);
	fdt_check_ret(fdt_begin_node(fdt, name), "its");
	fdt_check_ret(fdt_property_string(fdt, "compatible", "arm,gic-v3-its"),
		"its compatible");
	fdt_check_ret(fdt_property(fdt, "msi-controller", NULL, 0), "its msi-controller");
	fdt_check_ret(fdt_property_u32(fdt, "#msi-cells", 1U), "its msi-cells");
	fdt_check_ret(fdt_property_u32(fdt, "phandle", QEMU_FDT_PHANDLE_ITS), "its phandle");
	fdt_property_reg64(fdt, "reg", its_base, its_size);
	fdt_check_ret(fdt_end_node(fdt), "its end");
}

static void fdt_add_timer(void *fdt)
{
	static const char timer_compat[] = "arm,armv8-timer\0arm,armv7-timer";
	static const uint32_t interrupts[] = {
		QEMU_FDT_GIC_PPI, 13U, QEMU_FDT_IRQ_TYPE_LEVEL,
		QEMU_FDT_GIC_PPI, 14U, QEMU_FDT_IRQ_TYPE_LEVEL,
		QEMU_FDT_GIC_PPI, 11U, QEMU_FDT_IRQ_TYPE_LEVEL,
		QEMU_FDT_GIC_PPI, 10U, QEMU_FDT_IRQ_TYPE_LEVEL,
	};

	fdt_check_ret(fdt_begin_node(fdt, "timer"), "timer");
	fdt_check_ret(fdt_property(fdt, "compatible", timer_compat, sizeof(timer_compat)),
		"timer compatible");
	fdt_check_ret(fdt_property_u32(fdt, "interrupt-parent", QEMU_FDT_PHANDLE_GIC),
		"timer interrupt-parent");
	fdt_property_irq4(fdt, "interrupts", interrupts, ARRAY_SIZE(interrupts));
	fdt_check_ret(fdt_end_node(fdt), "timer end");
}

static void fdt_add_uart_clock(void *fdt)
{
	fdt_check_ret(fdt_begin_node(fdt, "apb-pclk"), "uartclk");
	fdt_check_ret(fdt_property_string(fdt, "compatible", "fixed-clock"), "uartclk compatible");
	fdt_check_ret(fdt_property_u32(fdt, "clock-frequency", QEMU_FDT_UART_CLOCK_HZ),
		"uartclk frequency");
	fdt_check_ret(fdt_property_u32(fdt, "#clock-cells", 0U), "uartclk clock-cells");
	fdt_check_ret(fdt_property_u32(fdt, "phandle", QEMU_FDT_PHANDLE_UARTCLK),
		"uartclk phandle");
	fdt_check_ret(fdt_end_node(fdt), "uartclk end");
}

static void fdt_add_uart(void *fdt, struct acrn_vm *vm)
{
	const struct arch_vm_config *arch_config = &get_vm_config(vm->vm_id)->arch;
	static const uint32_t interrupts[] = {
		QEMU_FDT_GIC_SPI, 1U, QEMU_FDT_IRQ_TYPE_LEVEL,
	};
	static const char uart_compat[] = "arm,pl011\0arm,primecell";
	char name[32];
	uint64_t uart_base = arch_config->guest_uart_base;

	snprintf(name, sizeof(name), "pl011@%lx", uart_base);
	fdt_check_ret(fdt_begin_node(fdt, name), "uart");
	fdt_check_ret(fdt_property(fdt, "compatible", uart_compat, sizeof(uart_compat)),
		"uart compatible");
	fdt_property_reg64(fdt, "reg", uart_base, arch_config->guest_uart_size);
	fdt_property_irq4(fdt, "interrupts", interrupts, ARRAY_SIZE(interrupts));
	fdt_check_ret(fdt_property_u32(fdt, "current-speed", QEMU_FDT_UART_BAUD), "uart baud");
	fdt_check_ret(fdt_property_u32(fdt, "clocks", QEMU_FDT_PHANDLE_UARTCLK), "uart clocks");
	fdt_check_ret(fdt_property_string(fdt, "clock-names", "uartclk"), "uart clock name");
	fdt_check_ret(fdt_property_string(fdt, "status", "okay"), "uart status");
	fdt_check_ret(fdt_property_u32(fdt, "phandle", QEMU_FDT_PHANDLE_UART), "uart phandle");
	fdt_check_ret(fdt_end_node(fdt), "uart end");
}

void arch_init_service_vm_vfdt(struct acrn_vm *vm)
{
	void *fdt = vm_get_vfdt(vm);

	fdt_check_ret(fdt_create(fdt, MAX_FDT_SIZE), "create");
	fdt_check_ret(fdt_finish_reservemap(fdt), "reservemap");
	fdt_check_ret(fdt_begin_node(fdt, ""), "root");
	fdt_check_ret(fdt_property_string(fdt, "model", "linux,dummy-virt"), "model");
	fdt_check_ret(fdt_property_string(fdt, "compatible", "linux,dummy-virt"), "compatible");
	fdt_check_ret(fdt_property_u32(fdt, "interrupt-parent", QEMU_FDT_PHANDLE_GIC),
		"interrupt-parent");
	fdt_check_ret(fdt_property_u32(fdt, "#address-cells", 2U), "address-cells");
	fdt_check_ret(fdt_property_u32(fdt, "#size-cells", 2U), "size-cells");

	fdt_add_chosen(fdt, vm);
	fdt_add_cpus(fdt, vm);
	fdt_add_psci(fdt);
	fdt_add_memory(fdt, vm);
	fdt_add_gic(fdt, vm);
	fdt_add_its(fdt, vm);
	fdt_add_timer(fdt);
	fdt_add_uart_clock(fdt);
	fdt_add_uart(fdt, vm);

	fdt_check_ret(fdt_end_node(fdt), "root end");
	fdt_check_ret(fdt_finish(fdt), "finish");
}
