/*
 * Copyright (C) 2026 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <cpu.h>
#include <errno.h>
#include <guest_memory.h>
#include <notify.h>
#include <per_cpu.h>
#include <rtl.h>
#include <sprintf.h>
#include <reloc.h>
#include <vm.h>
#include <vcpu.h>
#include <host_pm.h>
#include <schedule.h>
#include <debug/symbol.h>
#include <asm/mmu.h>
#include <asm/platform.h>
#include <asm/guest/vcpu.h>
#include "../shell_priv.h"

#define SHELL_CMD_MEM_MAP		"vmap"
#define SHELL_CMD_MEM_MAP_PARAM		NULL
#define SHELL_CMD_MEM_MAP_HELP		"list arm64 host stage-1 and vm stage-2 memory mappings"
#define SHELL_CMD_DUMPSTAT		"dumpstat"
#define SHELL_CMD_DUMPSTAT_PARAM	"[vm id]"
#define SHELL_CMD_DUMPSTAT_HELP		"dump arm64 vm registers, raw guest stack, and host stack"
#define SHELL_CMD_REBOOT		"reboot"
#define SHELL_CMD_REBOOT_PARAM		NULL
#define SHELL_CMD_REBOOT_HELP		"trigger a system reboot (immediately)"
#define SHELL_CMD_CRASH			"crash"
#define SHELL_CMD_CRASH_PARAM		NULL
#define SHELL_CMD_CRASH_HELP		"trigger an arm64 host exception and dump the exception stack"
#define SHELL_CRASH_FAULT_ADDR		0x80000000UL
#define DUMPSTAT_SMP_CALL_TIMEOUT_US	1000U
#define DUMPSTAT_STACK_DEPTH		16U

static int32_t shell_list_mem(__unused int32_t argc, __unused char **argv);
static int32_t shell_dumpstat(int32_t argc, char **argv);
static int32_t shell_reboot(__unused int32_t argc, __unused char **argv);
static int32_t shell_crash(int32_t argc, __unused char **argv);

struct shell_cmd arch_shell_cmds[] = {
	{
		.str		= SHELL_CMD_MEM_MAP,
		.cmd_param	= SHELL_CMD_MEM_MAP_PARAM,
		.help_str	= SHELL_CMD_MEM_MAP_HELP,
		.fcn		= shell_list_mem,
	},
	{
		.str		= SHELL_CMD_DUMPSTAT,
		.cmd_param	= SHELL_CMD_DUMPSTAT_PARAM,
		.help_str	= SHELL_CMD_DUMPSTAT_HELP,
		.fcn		= shell_dumpstat,
	},
	{
		.str		= SHELL_CMD_REBOOT,
		.cmd_param	= SHELL_CMD_REBOOT_PARAM,
		.help_str	= SHELL_CMD_REBOOT_HELP,
		.fcn		= shell_reboot,
	},
	{
		.str		= SHELL_CMD_CRASH,
		.cmd_param	= SHELL_CMD_CRASH_PARAM,
		.help_str	= SHELL_CMD_CRASH_HELP,
		.fcn		= shell_crash,
	},
};
uint32_t arch_shell_cmds_sz = ARRAY_SIZE(arch_shell_cmds);

static void shell_print_mem_header(void)
{
	shell_puts("domain      type       attr       va/ipa start        pa start           size\r\n");
	shell_puts("──────────  ─────────  ─────────  ──────────────────  ──────────────────  ──────────────────\r\n");
}

static void shell_print_mem_map(const char *domain, const char *type, const char *attr,
	uint64_t addr, uint64_t paddr, uint64_t size)
{
	char temp_str[MAX_STR_SIZE];

	snprintf(temp_str, MAX_STR_SIZE,
		"%-10s  %-9s  %-9s  0x%016lx  0x%016lx  0x%016lx\r\n",
		domain, type, attr, addr, paddr, size);
	shell_puts(temp_str);
}

static void shell_print_mem_special(const char *domain, const char *type, const char *attr,
	uint64_t addr, uint64_t size)
{
	char temp_str[MAX_STR_SIZE];

	snprintf(temp_str, MAX_STR_SIZE,
		"%-10s  %-9s  %-9s  0x%016lx  %-18s  0x%016lx\r\n",
		domain, type, attr, addr, "-", size);
	shell_puts(temp_str);
}

static void shell_print_host_maps(void)
{
	const struct arm64_mem_region *mmio_regions;
	const struct mem_region *rsvd_regions;
	uint32_t mmio_count;
	uint32_t rsvd_count;
	uint32_t idx;
	uint64_t hv_base = get_hv_image_base();

	mmio_regions = arm64_get_platform_mmio_regions(&mmio_count);
	for (idx = 0U; idx < mmio_count; idx++) {
		shell_print_mem_map("host s1", "mmio", "device",
			mmio_regions[idx].base, mmio_regions[idx].base, mmio_regions[idx].size);
	}

	shell_print_mem_map("host s1", "ram", "normal",
		arm64_get_phys_mem_start(), arm64_get_phys_mem_start(), arm64_get_phys_mem_size());
	shell_print_mem_map("host s1", "hv_image", "normal-x",
		hv_base, hv_base, get_hv_image_size());

	rsvd_regions = arm64_get_reserved_mem_regions(&rsvd_count);
	for (idx = 0U; idx < rsvd_count; idx++) {
		shell_print_mem_special("host s1", "rsvd", "unmapped",
			rsvd_regions[idx].addr, rsvd_regions[idx].size);
	}
}

static void shell_print_vm_stage2_maps(const struct acrn_vm *vm)
{
	char domain[16];

	snprintf(domain, sizeof(domain), "vm-%u s2", vm->vm_id);
	shell_print_mem_map(domain, "ram", "normal",
		arm64_platform_guest_ram_start(vm->vm_id),
		arm64_platform_guest_ram_hpa(vm->vm_id),
		arm64_platform_guest_ram_size(vm->vm_id));
	shell_print_mem_special(domain, "vgicd", "vio",
		arm64_platform_guest_gicd_base(vm->vm_id),
		arm64_platform_guest_gicd_size(vm->vm_id));
	shell_print_mem_special(domain, "vgicr", "vio",
		arm64_platform_guest_gicr_base(vm->vm_id),
		arm64_platform_guest_gicr_size(vm->vm_id));
	shell_print_mem_special(domain, "vpl011", "vio",
		arm64_platform_guest_uart_base(vm->vm_id),
		arm64_platform_guest_uart_size(vm->vm_id));
}

static int32_t shell_list_mem(__unused int32_t argc, __unused char **argv)
{
	uint16_t vm_id;

	shell_puts("\r\narm64 memory mappings:\r\n");
	shell_print_mem_header();
	shell_print_host_maps();

	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		struct acrn_vm *vm = get_vm_from_vmid(vm_id);

		if (vm->root_stg2ptp != NULL) {
			shell_print_vm_stage2_maps(vm);
		}
	}

	return 0;
}

static const char *thread_state_to_str(enum thread_object_state state)
{
	const char *str;

	switch (state) {
	case THREAD_STS_RUNNING:
		str = "running";
		break;
	case THREAD_STS_RUNNABLE:
		str = "runnable";
		break;
	case THREAD_STS_BLOCKED:
		str = "blocked";
		break;
	default:
		str = "unknown";
		break;
	}

	return str;
}

static const char *vcpu_sched_state_to_str(const struct acrn_vcpu *vcpu)
{
	return (vcpu->state == VCPU_OFFLINE) ?
		"offline" : thread_state_to_str(vcpu->thread_obj.status);
}

static void shell_dumpstat_regs(const struct cpu_regs *regs)
{
	shell_item_line("%-5s:0x%016lx  %-5s:0x%016lx  %-5s:0x%016lx",
		"elr", regs->elr, "spsr", regs->spsr, "esr", regs->esr);
	shell_item_line("%-5s:0x%016lx  %-5s:0x%016lx",
		"far", regs->far, "hpfar", regs->hpfar);
	shell_item_line("%-5s:0x%016lx  %-5s:0x%016lx  %-5s:0x%016lx  %-5s:0x%016lx",
		"x00", regs->x0, "x01", regs->x1, "x02", regs->x2, "x03", regs->x3);
	shell_item_line("%-5s:0x%016lx  %-5s:0x%016lx  %-5s:0x%016lx  %-5s:0x%016lx",
		"x04", regs->x4, "x05", regs->x5, "x06", regs->x6, "x07", regs->x7);
	shell_item_line("%-5s:0x%016lx  %-5s:0x%016lx  %-5s:0x%016lx  %-5s:0x%016lx",
		"x08", regs->x8, "x09", regs->x9, "x10", regs->x10, "x11", regs->x11);
	shell_item_line("%-5s:0x%016lx  %-5s:0x%016lx  %-5s:0x%016lx  %-5s:0x%016lx",
		"x12", regs->x12, "x13", regs->x13, "x14", regs->x14, "x15", regs->x15);
	shell_item_line("%-5s:0x%016lx  %-5s:0x%016lx  %-5s:0x%016lx  %-5s:0x%016lx",
		"x16", regs->x16, "x17", regs->x17, "x18", regs->x18, "x19", regs->x19);
	shell_item_line("%-5s:0x%016lx  %-5s:0x%016lx  %-5s:0x%016lx  %-5s:0x%016lx",
		"x20", regs->x20, "x21", regs->x21, "x22", regs->x22, "x23", regs->x23);
	shell_item_line("%-5s:0x%016lx  %-5s:0x%016lx  %-5s:0x%016lx  %-5s:0x%016lx",
		"x24", regs->x24, "x25", regs->x25, "x26", regs->x26, "x27", regs->x27);
	shell_item_line("%-5s:0x%016lx  %-5s:0x%016lx  %-5s:0x%016lx  %-5s:0x%016lx",
		"x28", regs->x28, "x29", regs->x29, "lr", regs->lr, "sp", regs->sp);
}

static bool shell_stack_contains(uint64_t start, uint64_t end, uint64_t addr, uint64_t bytes)
{
	uint64_t left;

	if ((addr < start) || (addr >= end)) {
		return false;
	}

	left = end - addr;
	return bytes <= left;
}

static void shell_dumpstat_print_frame(uint32_t idx, uint64_t fp, uint64_t lr, bool symbolize)
{
	char sym[96U];

	if (symbolize) {
		dbg_format_symbol(lr, sym, sizeof(sym));
		shell_item_line("[%02u] %-4s:0x%016lx  %-4s:0x%016lx  %s",
			idx, "fp", fp, "lr", lr, sym);
	} else {
		shell_item_line("[%02u] %-4s:0x%016lx  %-4s:0x%016lx",
			idx, "fp", fp, "lr", lr);
	}
}

static void shell_dumpstat_unwind_host_stack(uint64_t sp, uint64_t fp, uint64_t lr,
	uint64_t stack_start, uint64_t stack_end)
{
	uint32_t idx;

	shell_item_line("%-6s:0x%016lx", "sp", sp);
	shell_item_line("%-6s:0x%016lx-0x%016lx", "stack", stack_start, stack_end);

	if (!shell_stack_contains(stack_start, stack_end, sp, sizeof(uint64_t))) {
		shell_item_line("trace unavailable: sp is outside the stack");
		return;
	}

	for (idx = 0U; idx < DUMPSTAT_STACK_DEPTH; idx++) {
		uint64_t next_fp = 0UL;

		shell_dumpstat_print_frame(idx, fp, lr, true);

		if (!shell_stack_contains(stack_start, stack_end, fp, sizeof(uint64_t) * 2UL)) {
			break;
		}

		next_fp = *((const uint64_t *)fp);
		lr = *(((const uint64_t *)fp) + 1UL);
		if ((next_fp == SP_BOTTOM_MAGIC) || (next_fp <= fp)) {
			break;
		}

		fp = next_fp;
	}
}

struct dumpstat_guest_frame {
	uint64_t fp;
	uint64_t lr;
};

static void shell_dumpstat_vm_stack(struct acrn_vcpu *vcpu, const struct cpu_regs *regs)
{
	struct dumpstat_guest_frame frame;
	uint64_t fp = regs->x29;
	uint64_t lr = regs->lr;
	uint32_t idx;

	shell_item_line("%-6s:0x%016lx  %-6s:0x%016lx", "pc", regs->elr, "sp", regs->sp);

	if ((fp == 0UL) && (lr == 0UL)) {
		shell_item_line("trace unavailable: empty frame registers");
		return;
	}

	for (idx = 0U; idx < DUMPSTAT_STACK_DEPTH; idx++) {
		shell_dumpstat_print_frame(idx, fp, lr, false);

		if (fp == 0UL) {
			break;
		}

		if (copy_from_gpa(vcpu->vm, &frame, fp, sizeof(frame)) != 0) {
			shell_item_line("trace stopped: guest fp is not directly readable as GPA");
			break;
		}
		if ((frame.fp == 0UL) || (frame.fp <= fp)) {
			break;
		}

		fp = frame.fp;
		lr = frame.lr;
	}
}

static void shell_dumpstat_host_stack(const struct acrn_vcpu *vcpu)
{
	const struct stack_frame *saved_frame = (const struct stack_frame *)vcpu->thread_obj.host_sp;
	uint64_t stack_start = (uint64_t)&vcpu->stack[0];
	uint64_t stack_end = (uint64_t)&vcpu->stack[CONFIG_STACK_SIZE];

	if (!shell_stack_contains(stack_start, stack_end, vcpu->thread_obj.host_sp, sizeof(*saved_frame))) {
		shell_item_line("trace unavailable: saved sp is outside the vcpu thread stack");
		return;
	}

	shell_item_line("pcpu:%hu source:vcpu-thread", pcpuid_from_vcpu(vcpu));
	shell_dumpstat_unwind_host_stack(vcpu->thread_obj.host_sp,
		saved_frame->x29, saved_frame->lr, stack_start, stack_end);
}

struct dumpstat_snapshot {
	struct acrn_vcpu *vcpu;
	struct cpu_regs regs;
	bool captured;
};

static void shell_dumpstat_capture(void *data)
{
	struct dumpstat_snapshot *snapshot = (struct dumpstat_snapshot *)data;

	if (get_running_vcpu(get_pcpu_id()) == snapshot->vcpu) {
		(void)memcpy_s(&snapshot->regs, sizeof(snapshot->regs),
			&snapshot->vcpu->arch.regs, sizeof(snapshot->regs));
		snapshot->captured = true;
	}
}

static const struct cpu_regs *shell_dumpstat_get_regs(struct acrn_vcpu *vcpu,
	struct dumpstat_snapshot *snapshot)
{
	uint16_t pcpu_id = vcpu->thread_obj.pcpu_id;

	snapshot->vcpu = vcpu;
	snapshot->captured = false;

	if ((vcpu->state == VCPU_RUNNING) &&
		(sched_get_current(pcpu_id) == &vcpu->thread_obj) &&
		(pcpu_id != get_pcpu_id())) {
		(void)smp_call_function_timeout(1UL << pcpu_id, shell_dumpstat_capture,
			snapshot, DUMPSTAT_SMP_CALL_TIMEOUT_US);
	}

	return snapshot->captured ? &snapshot->regs : &vcpu->arch.regs;
}

static int32_t shell_dumpstat_vcpu(struct acrn_vcpu *vcpu)
{
	struct thread_object *current;
	struct dumpstat_snapshot snapshot;
	const struct cpu_regs *regs;

	current = (vcpu->state == VCPU_OFFLINE) ?
		NULL : sched_get_current(vcpu->thread_obj.pcpu_id);
	regs = shell_dumpstat_get_regs(vcpu, &snapshot);

	shell_item_begin("vm%hu/vcpu%hu", vcpu->vm->vm_id, vcpu->vcpu_id);
	shell_item_line("pcpu:%hu sched:%s current:%s live:%s", vcpu->thread_obj.pcpu_id,
		vcpu_sched_state_to_str(vcpu),
		(current == &vcpu->thread_obj) ? "yes" : "no",
		snapshot.captured ? "yes" : "no");
	shell_item_section("guest regs");
	shell_dumpstat_regs(regs);
	if (vcpu->state != VCPU_OFFLINE) {
		shell_item_section("guest stack symbols:none");
		shell_dumpstat_vm_stack(vcpu, regs);
		shell_item_section("host stack symbols:clan");
		shell_dumpstat_host_stack(vcpu);
	}
	shell_item_end();

	return 0;
}

static int32_t shell_dumpstat(int32_t argc, char **argv)
{
	struct acrn_vm *vm;
	int64_t param;
	uint16_t vm_id = 0U;
	uint16_t vcpu_id;

	if (argc > 2) {
		return -EINVAL;
	}

	if (argc == 2) {
		param = strtol_deci(argv[1]);
		if ((param < 0) || (param >= CONFIG_MAX_VM_NUM)) {
			return -EINVAL;
		}
		vm_id = (uint16_t)param;
	}

	vm = get_vm_from_vmid(vm_id);
	if (is_poweroff_vm(vm)) {
		return -EINVAL;
	}

	shell_item_begin("dumpstat vm%hu:%s", vm_id, vm->name);
	shell_item_line("vcpus:%hu", vm->hw.created_vcpus);
	shell_item_end();

	for (vcpu_id = 0U; vcpu_id < vm->hw.created_vcpus; vcpu_id++) {
		(void)shell_dumpstat_vcpu(vcpu_from_vid(vm, vcpu_id));
	}

	return 0;
}

static int32_t shell_reboot(__unused int32_t argc, __unused char **argv)
{
	reset_host(false);
	return 0;
}

static int32_t shell_crash(int32_t argc, __unused char **argv)
{
	volatile uint64_t *fault_addr = (volatile uint64_t *)SHELL_CRASH_FAULT_ADDR;
	uint64_t value;

	if (argc != 1) {
		return -EINVAL;
	}

	shell_puts("triggering arm64 host data abort...\r\n");
	value = *fault_addr;

	return (int32_t)value;
}
