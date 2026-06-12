/*
 * Copyright (C) 2021-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <vm.h>
#include <vboot.h>
#include <guest_memory.h>
#include <pgtable.h>
#include <errno.h>
#include <logmsg.h>
#include <util.h>
#if defined(CONFIG_ARM64)
#include <mmu.h>
#include <asm/platform.h>
#endif

static bool range_overlaps(uint64_t start_a, uint64_t size_a, uint64_t start_b, uint64_t size_b)
{
	uint64_t end_a = start_a + size_a;
	uint64_t end_b = start_b + size_b;

	return (end_a > start_a) && (end_b > start_b) && (start_a < end_b) && (start_b < end_a);
}

#if defined(CONFIG_ARM64)
static uint64_t arm64_rawimage_fdt_load_gpa(struct acrn_vm *vm, uint64_t kernel_load_gpa,
	uint32_t kernel_size)
{
	uint64_t ram_start = arm64_platform_guest_ram_start(vm->vm_id);
	uint64_t ram_size = arm64_platform_guest_ram_size(vm->vm_id);
	uint64_t fdt_size = roundup((uint64_t)vm->sw.fdt_info.size, MEM_4K);
	uint64_t fdt_load_gpa = ram_start;

	if (fdt_size == 0UL) {
		fdt_size = MEM_4K;
	}

	/*
	 * Static RTOS images do not have a Linux bootloader to choose safe module
	 * addresses. Prefer the start of the VM RAM window for the synthetic FDT;
	 * if that collides with the raw image, move it to the end of the same RAM
	 * window. Returning 0 makes overlap explicit instead of silently corrupting
	 * the kernel image.
	 */
	if (range_overlaps(fdt_load_gpa, fdt_size, kernel_load_gpa, kernel_size)) {
		if ((ram_size <= fdt_size) || ((ram_start + ram_size) <= ram_start)) {
			return 0UL;
		}
		fdt_load_gpa = (ram_start + ram_size - fdt_size) & ~(uint64_t)(MEM_4K - 1U);
	}

	if ((fdt_load_gpa < ram_start) ||
		((fdt_load_gpa + fdt_size) > (ram_start + ram_size)) ||
		range_overlaps(fdt_load_gpa, fdt_size, kernel_load_gpa, kernel_size)) {
		return 0UL;
	}

	return fdt_load_gpa;
}
#endif

/**
 * @pre vm != NULL
 */
static int32_t load_rawimage(struct acrn_vm *vm)
{
	struct sw_kernel_info *sw_kernel = &(vm->sw.kernel_info);
	const struct acrn_vm_config *vm_config = get_vm_config(vm->vm_id);
	uint64_t kernel_load_gpa;
	int32_t ret;

	/* TODO: GPA 0 load support */
	kernel_load_gpa = vm_config->os_config.kernel_load_addr;

	/* TODO: For simplicity assume there are enough space just before kernel load address
	 * Fix this after implementing find_space_from_vm_vfdt API
	 */
	if (vm->sw.fdt_info.src_addr != NULL) {
#if defined(CONFIG_ARM64)
		uint64_t fdt_load_gpa = arm64_rawimage_fdt_load_gpa(vm, kernel_load_gpa,
			sw_kernel->kernel_size);

		if (fdt_load_gpa == 0UL) {
			pr_err("vm-%u fdt does not fit guest ram without overlapping raw image",
				vm->vm_id);
			return -EFAULT;
		}
		vm->sw.fdt_info.load_addr = (void *)fdt_load_gpa;
		pr_info("vm-%u fdt placed at gpa[0x%lx-0x%lx]",
			vm->vm_id, fdt_load_gpa, fdt_load_gpa + vm->sw.fdt_info.size);
#else
		vm->sw.fdt_info.load_addr = (void *)0x40000000UL;
#endif
	}

	/* Copy the guest kernel image to its run-time location */
	ret = copy_to_gpa(vm, sw_kernel->kernel_src_addr, kernel_load_gpa, sw_kernel->kernel_size);
	if (ret == 0) {
		pr_info("vm-%u image %s copied to 1:1 ram gpa[0x%lx-0x%lx]",
			vm->vm_id, vm_config->os_config.kernel_mod_tag, kernel_load_gpa,
			kernel_load_gpa + sw_kernel->kernel_size);
	} else {
		pr_err("vm-%u image %s does not fit 1:1 ram gpa[0x%lx-0x%lx]",
			vm->vm_id, vm_config->os_config.kernel_mod_tag, kernel_load_gpa,
			kernel_load_gpa + sw_kernel->kernel_size);
		return -EFAULT;
	}

	sw_kernel->kernel_entry_addr = (void *)vm_config->os_config.kernel_entry_addr;
	return 0;
}

int32_t rawimage_loader(struct acrn_vm *vm)
{
	return load_rawimage(vm);
}
