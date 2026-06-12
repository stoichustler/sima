/*
 * Copyright (C) 2026 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <cpu.h>
#include <errno.h>
#include <notify.h>
#include <per_cpu.h>
#include <rtl.h>
#include <sprintf.h>
#include <reloc.h>
#include <vm.h>
#include <vcpu.h>
#include <host_pm.h>
#include <schedule.h>
#include <ticks.h>
#include <asm/mmu.h>
#include <asm/platform.h>
#include <asm/guest/vcpu.h>
#include "../shell_priv.h"

#define SHELL_CMD_MEM_MAP		"vmap"
#define SHELL_CMD_MEM_MAP_PARAM		NULL
#define SHELL_CMD_MEM_MAP_HELP		"list arm64 host stage-1 and vm stage-2 memory mappings"
#define SHELL_CMD_KDUMP			"vdump"
#define SHELL_CMD_KDUMP_PARAM		"[vm id] [vcpu id]"
#define SHELL_CMD_KDUMP_HELP		"dump arm64 vm vcpu registers, thread state, and stack words"
#define SHELL_CMD_REBOOT		"reboot"
#define SHELL_CMD_REBOOT_PARAM		NULL
#define SHELL_CMD_REBOOT_HELP		"trigger a system reboot (immediately)"
#define SHELL_CMD_CRASH			"crash"
#define SHELL_CMD_CRASH_PARAM		NULL
#define SHELL_CMD_CRASH_HELP		"trigger an arm64 host exception and dump the exception stack"
#define SHELL_CMD_BENCH			"bench"
#define SHELL_CMD_BENCH_PARAM		"[loops] [mem_kb]"
#define SHELL_CMD_BENCH_HELP		"run a clan cpu/mem stress loop and print cpu/mem occupancy"
#define SHELL_CRASH_FAULT_ADDR		0x80000000UL
#define KDUMP_REMOTE_LIVE_CAPTURE	0
#define KDUMP_SMP_CALL_TIMEOUT_US	1000U
#define BENCH_DEFAULT_LOOPS		1000UL
#define BENCH_MAX_LOOPS			1000000UL
#define BENCH_DEFAULT_MEM_KB		64UL
#define BENCH_SCRATCH_KB		256UL
#define BENCH_LINE_BYTES		64UL

static int32_t shell_list_mem(__unused int32_t argc, __unused char **argv);
static int32_t shell_kdump(int32_t argc, char **argv);
static int32_t shell_reboot(__unused int32_t argc, __unused char **argv);
static int32_t shell_crash(int32_t argc, __unused char **argv);
static int32_t shell_bench(int32_t argc, char **argv);

static uint8_t bench_scratch[BENCH_SCRATCH_KB * 1024UL] __aligned(64);
static volatile uint64_t bench_sink;

struct shell_cmd arch_shell_cmds[] = {
	{
		.str		= SHELL_CMD_MEM_MAP,
		.cmd_param	= SHELL_CMD_MEM_MAP_PARAM,
		.help_str	= SHELL_CMD_MEM_MAP_HELP,
		.fcn		= shell_list_mem,
	},
	{
		.str		= SHELL_CMD_KDUMP,
		.cmd_param	= SHELL_CMD_KDUMP_PARAM,
		.help_str	= SHELL_CMD_KDUMP_HELP,
		.fcn		= shell_kdump,
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
	{
		.str		= SHELL_CMD_BENCH,
		.cmd_param	= SHELL_CMD_BENCH_PARAM,
		.help_str	= SHELL_CMD_BENCH_HELP,
		.fcn		= shell_bench,
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

static uint64_t shell_vm_backing_bytes(void)
{
	uint64_t bytes = 0UL;
	uint16_t vm_id;

	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		struct acrn_vm *vm = get_vm_from_vmid(vm_id);

		if (!is_poweroff_vm(vm) && (vm->root_stg2ptp != NULL)) {
			bytes += arm64_platform_guest_ram_size(vm_id);
		}
	}

	return bytes;
}

static uint64_t shell_reserved_bytes(void)
{
	const struct mem_region *rsvd_regions;
	uint32_t rsvd_count;
	uint32_t idx;
	uint64_t bytes = 0UL;

	rsvd_regions = arm64_get_reserved_mem_regions(&rsvd_count);
	for (idx = 0U; idx < rsvd_count; idx++) {
		bytes += rsvd_regions[idx].size;
	}

	return bytes;
}

static void shell_bench_print_cpu(uint64_t loops, uint64_t delta_ticks)
{
	char temp_str[MAX_STR_SIZE];
	uint64_t ticks_per_loop = (loops != 0UL) ? (delta_ticks / loops) : 0UL;
	uint64_t loops_per_ms = 0UL;
	uint16_t pcpu_id;
	uint16_t pcpu_num = get_pcpu_nums();

	if (delta_ticks != 0UL) {
		loops_per_ms = (loops * (uint64_t)arch_cpu_tickrate()) / delta_ticks;
	}

	snprintf(temp_str, MAX_STR_SIZE,
		"cpu bench loops:%lu ticks:%lu us:%lu tick_khz:%u loops/ms:%lu ticks/loop:%lu\r\n",
		loops, delta_ticks, ticks_to_us(delta_ticks), arch_cpu_tickrate(),
		loops_per_ms, ticks_per_loop);
	shell_puts(temp_str);
	shell_puts("pcpu    active    current_thread   state\r\n");
	shell_puts("────    ──────    ──────────────   ────────\r\n");

	for (pcpu_id = 0U; pcpu_id < pcpu_num; pcpu_id++) {
		struct thread_object *current = sched_get_current(pcpu_id);
		const char *name = (current != NULL) ? current->name : "-";
		const char *state = (current != NULL) ?
			(is_idle_thread(current) ? "idle" : "busy") : "-";

		snprintf(temp_str, MAX_STR_SIZE, "%-7hu %-9s %-16s %s\r\n",
			pcpu_id, is_pcpu_active(pcpu_id) ? "yes" : "no", name, state);
		shell_puts(temp_str);
	}
}

static void shell_bench_print_mem(uint64_t mem_bytes, uint64_t bytes_touched)
{
	char temp_str[MAX_STR_SIZE];
	uint64_t host_ram = arm64_get_phys_mem_size();
	uint64_t hv_image = get_hv_image_size();
	uint64_t vm_backing = shell_vm_backing_bytes();
	uint64_t reserved = shell_reserved_bytes();
	uint64_t bench_reserved = sizeof(bench_scratch);
	uint64_t visible_used = hv_image + vm_backing + reserved + bench_reserved;
	uint64_t visible_pct = (host_ram != 0UL) ? ((visible_used * 100UL) / host_ram) : 0UL;

	snprintf(temp_str, MAX_STR_SIZE,
		"mem bench scratch:%luKB touched:%luKB total_touched:%luKB\r\n",
		mem_bytes / 1024UL, mem_bytes == 0UL ? 0UL : mem_bytes / 1024UL,
		bytes_touched / 1024UL);
	shell_puts(temp_str);
	snprintf(temp_str, MAX_STR_SIZE,
		"mem occupancy host_ram:%luKB visible_used:%luKB used:%lu%% hv:%luKB vm_backing:%luKB reserved:%luKB bench:%luKB\r\n",
		host_ram / 1024UL, visible_used / 1024UL, visible_pct,
		hv_image / 1024UL, vm_backing / 1024UL, reserved / 1024UL,
		bench_reserved / 1024UL);
	shell_puts(temp_str);
}

static uint64_t shell_bench_run(uint64_t loops, uint64_t mem_bytes, uint64_t *bytes_touched)
{
	uint64_t acc = bench_sink;
	uint64_t loop;

	*bytes_touched = 0UL;
	for (loop = 0UL; loop < loops; loop++) {
		uint64_t off;

		acc ^= (loop << 7U) | (loop >> 3U);
		acc += 0x9e3779b97f4a7c15UL;
		acc ^= acc >> 29U;

		for (off = 0UL; off < mem_bytes; off += BENCH_LINE_BYTES) {
			bench_scratch[off] = (uint8_t)(acc + off + loop);
			acc += bench_scratch[off];
		}

		*bytes_touched += mem_bytes;
	}

	bench_sink = acc;
	return acc;
}

static int32_t shell_bench(int32_t argc, char **argv)
{
	char temp_str[MAX_STR_SIZE];
	uint64_t loops = BENCH_DEFAULT_LOOPS;
	uint64_t mem_kb = BENCH_DEFAULT_MEM_KB;
	int64_t param;
	uint64_t mem_bytes;
	uint64_t bytes_touched;
	uint64_t start;
	uint64_t delta;
	uint64_t result;

	if (argc > 3) {
		return -EINVAL;
	}

	if (argc >= 2) {
		param = strtol_deci(argv[1]);
		if (param < 0) {
			return -EINVAL;
		}
		loops = (uint64_t)param;
	}
	if (argc == 3) {
		param = strtol_deci(argv[2]);
		if (param < 0) {
			return -EINVAL;
		}
		mem_kb = (uint64_t)param;
	}

	if (loops > BENCH_MAX_LOOPS) {
		loops = BENCH_MAX_LOOPS;
	}
	if (mem_kb > BENCH_SCRATCH_KB) {
		mem_kb = BENCH_SCRATCH_KB;
	}

	mem_bytes = mem_kb * 1024UL;
	mem_bytes &= ~(BENCH_LINE_BYTES - 1UL);

	snprintf(temp_str, MAX_STR_SIZE,
		"\r\nbench start loops:%lu mem:%luKB max_loops:%lu max_mem:%luKB\r\n",
		loops, mem_bytes / 1024UL, BENCH_MAX_LOOPS, BENCH_SCRATCH_KB);
	shell_puts(temp_str);

	start = cpu_ticks();
	result = shell_bench_run(loops, mem_bytes, &bytes_touched);
	delta = cpu_ticks() - start;

	shell_bench_print_cpu(loops, delta);
	shell_bench_print_mem(mem_bytes, bytes_touched);
	snprintf(temp_str, MAX_STR_SIZE, "bench result:0x%016lx\r\n", result);
	shell_puts(temp_str);

	return 0;
}

static const char *vcpu_state_to_str(enum vcpu_state state)
{
	const char *str;

	switch (state) {
	case VCPU_OFFLINE:
		str = "offline";
		break;
	case VCPU_INIT:
		str = "init";
		break;
	case VCPU_RUNNING:
		str = "running";
		break;
	case VCPU_ZOMBIE:
		str = "zombie";
		break;
	default:
		str = "unknown";
		break;
	}

	return str;
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

static void shell_kdump_regs(const struct cpu_regs *regs)
{
	char temp_str[MAX_STR_SIZE];

	snprintf(temp_str, MAX_STR_SIZE,
		"λ  elr:0x%016lx spsr:0x%016lx esr:0x%016lx far:0x%016lx hpfar:0x%016lx\r\n",
		regs->elr, regs->spsr, regs->esr, regs->far, regs->hpfar);
	shell_puts(temp_str);
	snprintf(temp_str, MAX_STR_SIZE,
		"λ   x0:0x%016lx   x1:0x%016lx  x2:0x%016lx  x3:0x%016lx\r\n",
		regs->x0, regs->x1, regs->x2, regs->x3);
	shell_puts(temp_str);
	snprintf(temp_str, MAX_STR_SIZE,
		"λ   x4:0x%016lx   x5:0x%016lx  x6:0x%016lx  x7:0x%016lx\r\n",
		regs->x4, regs->x5, regs->x6, regs->x7);
	shell_puts(temp_str);
	snprintf(temp_str, MAX_STR_SIZE,
		"λ   x8:0x%016lx   x9:0x%016lx x10:0x%016lx x11:0x%016lx\r\n",
		regs->x8, regs->x9, regs->x10, regs->x11);
	shell_puts(temp_str);
	snprintf(temp_str, MAX_STR_SIZE,
		"λ  x12:0x%016lx  x13:0x%016lx x14:0x%016lx x15:0x%016lx\r\n",
		regs->x12, regs->x13, regs->x14, regs->x15);
	shell_puts(temp_str);
	snprintf(temp_str, MAX_STR_SIZE,
		"λ  x16:0x%016lx  x17:0x%016lx x18:0x%016lx x19:0x%016lx\r\n",
		regs->x16, regs->x17, regs->x18, regs->x19);
	shell_puts(temp_str);
	snprintf(temp_str, MAX_STR_SIZE,
		"λ  x20:0x%016lx  x21:0x%016lx x22:0x%016lx x23:0x%016lx\r\n",
		regs->x20, regs->x21, regs->x22, regs->x23);
	shell_puts(temp_str);
	snprintf(temp_str, MAX_STR_SIZE,
		"λ  x24:0x%016lx  x25:0x%016lx x26:0x%016lx x27:0x%016lx\r\n",
		regs->x24, regs->x25, regs->x26, regs->x27);
	shell_puts(temp_str);
	snprintf(temp_str, MAX_STR_SIZE,
		"λ  x28:0x%016lx  x29:0x%016lx  lr:0x%016lx  sp:0x%016lx\r\n",
		regs->x28, regs->x29, regs->lr, regs->sp);
	shell_puts(temp_str);
}

static void shell_kdump_stack(const struct acrn_vcpu *vcpu)
{
	const uint64_t *sp = (const uint64_t *)vcpu->thread_obj.host_sp;
	uint64_t stack_start = (uint64_t)&vcpu->stack[0];
	uint64_t stack_end = (uint64_t)&vcpu->stack[CONFIG_STACK_SIZE];
	char temp_str[MAX_STR_SIZE];
	uint32_t words;
	uint32_t idx;

	snprintf(temp_str, MAX_STR_SIZE, "thread sp:0x%016lx stack:[0x%016lx-0x%016lx]\r\n",
		vcpu->thread_obj.host_sp, stack_start, stack_end);
	shell_puts(temp_str);

	if (((uint64_t)sp < stack_start) || ((uint64_t)sp >= stack_end)) {
		shell_puts("thread sp is outside the vcpu stack\r\n");
		return;
	}

	words = (uint32_t)((stack_end - (uint64_t)sp) / sizeof(uint64_t));
	if (words > 8U) {
		words = 8U;
	}

	for (idx = 0U; idx < words; idx += 2U) {
		uint64_t next = ((idx + 1U) < words) ? sp[idx + 1U] : 0UL;

		snprintf(temp_str, MAX_STR_SIZE, "stack[%u] 0x%016lx: 0x%016lx 0x%016lx\r\n",
			idx / 2U, (uint64_t)&sp[idx], sp[idx], next);
		shell_puts(temp_str);
	}
}

struct kdump_snapshot {
	struct acrn_vcpu *vcpu;
	struct cpu_regs regs;
	bool captured;
};

#if KDUMP_REMOTE_LIVE_CAPTURE
static void shell_kdump_capture(void *data)
{
	struct kdump_snapshot *snapshot = (struct kdump_snapshot *)data;

	if (get_running_vcpu(get_pcpu_id()) == snapshot->vcpu) {
		(void)memcpy_s(&snapshot->regs, sizeof(snapshot->regs),
			&snapshot->vcpu->arch.regs, sizeof(snapshot->regs));
		snapshot->captured = true;
	}
}
#endif

static const struct cpu_regs *shell_kdump_get_regs(struct acrn_vcpu *vcpu,
	struct kdump_snapshot *snapshot)
{
	uint16_t pcpu_id = vcpu->thread_obj.pcpu_id;

	snapshot->vcpu = vcpu;
	snapshot->captured = false;

#if KDUMP_REMOTE_LIVE_CAPTURE
	if ((vcpu->state == VCPU_RUNNING) &&
		(sched_get_current(pcpu_id) == &vcpu->thread_obj) &&
		(pcpu_id != get_pcpu_id())) {
		(void)smp_call_function_timeout(1UL << pcpu_id, shell_kdump_capture,
			snapshot, KDUMP_SMP_CALL_TIMEOUT_US);
	}
#else
	(void)pcpu_id;
#endif

	return snapshot->captured ? &snapshot->regs : &vcpu->arch.regs;
}

static int32_t shell_kdump(int32_t argc, char **argv)
{
	struct acrn_vm *vm;
	struct acrn_vcpu *vcpu;
	struct thread_object *current;
	struct kdump_snapshot snapshot;
	const struct cpu_regs *regs;
	char temp_str[MAX_STR_SIZE];
	uint16_t vm_id = 0U;
	uint16_t vcpu_id = 0U;

	if (argc > 3) {
		return -EINVAL;
	}
	if (argc >= 2) {
		vm_id = sanitize_vmid((uint16_t)strtol_deci(argv[1]));
	}
	if (argc == 3) {
		vcpu_id = (uint16_t)strtol_deci(argv[2]);
	}

	vm = get_vm_from_vmid(vm_id);
	if (is_poweroff_vm(vm) || (vcpu_id >= vm->hw.created_vcpus)) {
		return -EINVAL;
	}

	vcpu = vcpu_from_vid(vm, vcpu_id);
	current = sched_get_current(vcpu->thread_obj.pcpu_id);
	regs = shell_kdump_get_regs(vcpu, &snapshot);

	snprintf(temp_str, MAX_STR_SIZE,
		"\r\nkdump vm%hu vcpu%hu name:%s pcpu:%hu vcpu_state:%s thread:%s current:%s live:%s\r\n",
		vm_id, vcpu_id, vm->name, vcpu->thread_obj.pcpu_id,
		vcpu_state_to_str(vcpu->state),
		thread_state_to_str(vcpu->thread_obj.status),
		(current == &vcpu->thread_obj) ? "yes" : "no",
		snapshot.captured ? "yes" : "no");
	shell_puts(temp_str);
	shell_kdump_regs(regs);
	shell_kdump_stack(vcpu);

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
