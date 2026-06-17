/*
 * Copyright (C) 2026 Hustler Lo.
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
#include <util.h>
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
#define DUMPSTAT_SMP_CALL_TIMEOUT_US	1000U
#define DUMPSTAT_STACK_DEPTH		16U
#define DUMPSTAT_REG_KEY_FMT		"%5s:0x%016lx"
#define DUMPSTAT_REGS_PER_LINE_MAX	4U
#define DUMPSTAT_VTIMER_VIRQ		ARM64_GIC_PPI_VIRTUAL_TIMER
#define DUMPSTAT_LOCAL_IRQ_NUM		ARM64_VGIC_LOCAL_IRQ_NUM
#define DUMPSTAT_CPUIF_ICC_PMR_EL1	0U
#define DUMPSTAT_CPUIF_ICC_CTLR_EL1	1U
#define DUMPSTAT_CPUIF_ICC_SRE_EL1	2U
#define DUMPSTAT_CPUIF_ICC_IGRPEN1_EL1	3U
#define DUMPSTAT_CPUIF_ICC_DIR_EL1	4U
#define DUMPSTAT_CPUIF_ICC_RPR_EL1	5U

static int32_t shell_list_mem(__unused int32_t argc, __unused char **argv);
static int32_t shell_dumpstat(int32_t argc, char **argv);
static int32_t shell_reboot(__unused int32_t argc, __unused char **argv);

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

static void shell_dumpstat_format_reg(char *buf, size_t size, const char *name,
	uint64_t value)
{
	snprintf(buf, size, DUMPSTAT_REG_KEY_FMT, name, value);
}

static void shell_dumpstat_reg_line(uint32_t count, ...)
{
	char reg[DUMPSTAT_REGS_PER_LINE_MAX][32U];
	va_list args;
	uint32_t idx;

	if ((count == 0U) || (count > DUMPSTAT_REGS_PER_LINE_MAX)) {
		return;
	}

	va_start(args, count);
	for (idx = 0U; idx < count; idx++) {
		const char *name = __builtin_va_arg(args, const char *);
		uint64_t value = __builtin_va_arg(args, uint64_t);

		shell_dumpstat_format_reg(reg[idx], sizeof(reg[idx]), name, value);
	}
	va_end(args);

	switch (count) {
	case 1U:
		shell_item_line("%s", reg[0U]);
		break;
	case 2U:
		shell_item_line("%s %s", reg[0U], reg[1U]);
		break;
	case 3U:
		shell_item_line("%s %s %s", reg[0U], reg[1U], reg[2U]);
		break;
	default:
		shell_item_line("%s %s %s %s", reg[0U], reg[1U], reg[2U], reg[3U]);
		break;
	}
}

static void shell_dumpstat_regs(const struct cpu_regs *regs)
{
	shell_dumpstat_reg_line(3U, "elr", regs->elr, "spsr", regs->spsr, "esr", regs->esr);
	shell_dumpstat_reg_line(2U, "far", regs->far, "hpfar", regs->hpfar);
	shell_dumpstat_reg_line(4U, "x00", regs->x0, "x01", regs->x1, "x02", regs->x2, "x03", regs->x3);
	shell_dumpstat_reg_line(4U, "x04", regs->x4, "x05", regs->x5, "x06", regs->x6, "x07", regs->x7);
	shell_dumpstat_reg_line(4U, "x08", regs->x8, "x09", regs->x9, "x10", regs->x10, "x11", regs->x11);
	shell_dumpstat_reg_line(4U, "x12", regs->x12, "x13", regs->x13, "x14", regs->x14, "x15", regs->x15);
	shell_dumpstat_reg_line(4U, "x16", regs->x16, "x17", regs->x17, "x18", regs->x18, "x19", regs->x19);
	shell_dumpstat_reg_line(4U, "x20", regs->x20, "x21", regs->x21, "x22", regs->x22, "x23", regs->x23);
	shell_dumpstat_reg_line(4U, "x24", regs->x24, "x25", regs->x25, "x26", regs->x26, "x27", regs->x27);
	shell_dumpstat_reg_line(4U, "x28", regs->x28, "x29", regs->x29, "lr", regs->lr, "sp", regs->sp);
}

static const char *shell_yes_no(bool value)
{
	return value ? "yes" : "no";
}

static const char *shell_exit_source_to_str(uint32_t source)
{
	const char *str;

	switch (source) {
	case ARM64_VCPU_DEBUG_EXIT_SYNC:
		str = "sync";
		break;
	case ARM64_VCPU_DEBUG_EXIT_IRQ:
		str = "irq";
		break;
	default:
		str = "none";
		break;
	}

	return str;
}

static const char *shell_abort_type_to_str(uint32_t abort_type)
{
	const char *str;

	switch (abort_type) {
	case ARM64_VCPU_DEBUG_ABORT_INSTRUCTION:
		str = "instruction";
		break;
	case ARM64_VCPU_DEBUG_ABORT_DATA:
		str = "data";
		break;
	default:
		str = "none";
		break;
	}

	return str;
}

static const char *shell_vgic_source_to_str(uint32_t source)
{
	const char *str;

	switch (source) {
	case ARM64_VCPU_DEBUG_VGIC_SYNC:
		str = "sync";
		break;
	case ARM64_VCPU_DEBUG_VGIC_MAINTENANCE:
		str = "maintenance";
		break;
	default:
		str = "none";
		break;
	}

	return str;
}

static const char *shell_cpuif_sysreg_to_str(uint32_t sysreg)
{
	const char *str;

	switch (sysreg) {
	case DUMPSTAT_CPUIF_ICC_SRE_EL1:
		str = "ICC_SRE_EL1";
		break;
	case DUMPSTAT_CPUIF_ICC_PMR_EL1:
		str = "ICC_PMR_EL1";
		break;
	case DUMPSTAT_CPUIF_ICC_CTLR_EL1:
		str = "ICC_CTLR_EL1";
		break;
	case DUMPSTAT_CPUIF_ICC_IGRPEN1_EL1:
		str = "ICC_IGRPEN1_EL1";
		break;
	case DUMPSTAT_CPUIF_ICC_DIR_EL1:
		str = "ICC_DIR_EL1";
		break;
	case DUMPSTAT_CPUIF_ICC_RPR_EL1:
		str = "ICC_RPR_EL1";
		break;
	default:
		str = "unknown";
		break;
	}

	return str;
}

static const char *shell_vtimer_trace_event_to_str(uint32_t event)
{
	const char *str;

	switch (event) {
	case ARM64_VTIMER_TRACE_LOAD:
		str = "load";
		break;
	case ARM64_VTIMER_TRACE_UNLOAD:
		str = "unload";
		break;
	case ARM64_VTIMER_TRACE_SYSREG:
		str = "sysreg";
		break;
	case ARM64_VTIMER_TRACE_PPI:
		str = "ppi";
		break;
	case ARM64_VTIMER_TRACE_POLL:
		str = "poll";
		break;
	case ARM64_VTIMER_TRACE_UPDATE:
		str = "update";
		break;
	case ARM64_VTIMER_TRACE_INJECT:
		str = "inject";
		break;
	case ARM64_VTIMER_TRACE_EOI:
		str = "eoi";
		break;
	case ARM64_VTIMER_TRACE_REQUEUE:
		str = "requeue";
		break;
	case ARM64_VTIMER_TRACE_BACKUP:
		str = "backup";
		break;
	default:
		str = "unknown";
		break;
	}

	return str;
}

static void shell_dumpstat_vgic_event(const char *label,
	const struct arm64_vcpu_last_vgic *last)
{
	/*
	 * These lines expose the last hardware/software vGIC sync boundary so a
	 * stalled guest can be classified without adding noisy interrupt logs.
	 */
	if (last->tsc == 0UL) {
		shell_item_line("%s:none", label);
	} else {
		shell_item_line("%s:source:%s count:%u used_lrs:%u tsc:0x%lx",
			label, shell_vgic_source_to_str(last->source),
			last->count, last->used_lrs, last->tsc);
		shell_item_line("       misr:0x%016lx eisr:0x%016lx elrsr:0x%016lx",
			last->misr, last->eisr, last->elrsr);
		shell_item_line("       hcr:0x%016lx vmcr:0x%016lx ap0r0:0x%016lx ap1r0:0x%016lx",
			last->hcr, last->vmcr, last->ap0r0, last->ap1r0);
		shell_item_line("       lr0:0x%016lx lr1:0x%016lx",
			last->lr0, last->lr1);
	}
}

static void shell_dumpstat_recent_events(const struct arm64_vcpu_debug_info *debug)
{
	const struct arm64_vcpu_last_exit *last_exit = &debug->last_exit;
	const struct arm64_vcpu_last_irq *last_irq = &debug->last_irq;
	const struct arm64_vcpu_last_timer *last_timer = &debug->last_timer;
	const struct arm64_vcpu_last_sgi *last_sgi = &debug->last_sgi;
	const struct arm64_vcpu_last_sgi_target *last_sgi_target = &debug->last_sgi_target;
	const struct arm64_vcpu_last_psci *last_psci = &debug->last_psci;
	const struct arm64_vcpu_last_cpuif *last_cpuif = &debug->last_cpuif;
	const struct arm64_vcpu_last_wfx *last_wfx = &debug->last_wfx;
	const struct arm64_vcpu_last_guest_return *last_return = &debug->last_return;

	if (last_exit->tsc == 0UL) {
		shell_item_line(" exit:none");
	} else {
		if (last_exit->ec == ARM64_VCPU_DEBUG_EXIT_EC_INVALID) {
			shell_item_line(" exit:source:%s ec:N/A status:%d tsc:0x%lx",
				shell_exit_source_to_str(last_exit->source),
				last_exit->status, last_exit->tsc);
		} else {
			shell_item_line(" exit:source:%s ec:0x%x status:%d tsc:0x%lx",
				shell_exit_source_to_str(last_exit->source),
				last_exit->ec, last_exit->status, last_exit->tsc);
		}
		shell_item_line("  esr:0x%016lx elr:0x%016lx far:0x%016lx hpfar:0x%016lx",
			last_exit->esr, last_exit->elr, last_exit->far, last_exit->hpfar);
		if (last_exit->abort_type != ARM64_VCPU_DEBUG_ABORT_NONE) {
			shell_item_line("  abort:%s fsc:0x%02x",
				shell_abort_type_to_str(last_exit->abort_type),
				last_exit->abort_fsc);
		}
	}

	if (last_irq->tsc == 0UL) {
		shell_item_line("  irq:none");
	} else {
		shell_item_line("  irq:virq:%u level:%s source-vcpu:%hu target-vcpu:%hu status:%d tsc:0x%lx",
			last_irq->virq, shell_yes_no(last_irq->level),
			last_irq->source_vcpu_id, last_irq->target_vcpu_id,
			last_irq->status, last_irq->tsc);
	}

	if (last_timer->tsc == 0UL) {
		shell_item_line("timer:none");
	} else {
		shell_item_line("timer:virq:%u sysreg:0x%x write:%s injected:%s status:%d tsc:0x%lx",
			last_timer->virq, last_timer->sysreg,
			shell_yes_no(last_timer->write), shell_yes_no(last_timer->injected),
			last_timer->status, last_timer->tsc);
		shell_item_line("       ctl:0x%08x cval:0x%016lx",
			last_timer->ctl, last_timer->cval);
	}

	if (last_sgi->tsc == 0UL) {
		shell_item_line("  sgi:none");
	} else {
		shell_item_line("  sgi:intid:%u source-vcpu:%hu target-mask:0x%04x delivered:0x%04x status:%d tsc:0x%lx",
			last_sgi->intid, last_sgi->source_vcpu_id,
			last_sgi->target_mask, last_sgi->delivered_mask,
			last_sgi->status, last_sgi->tsc);
		shell_item_line("      value:0x%016lx", last_sgi->value);
	}

	if (last_sgi_target->tsc == 0UL) {
		shell_item_line("sgi-target:none");
	} else {
		shell_item_line("sgi-target:intid:%u source-vcpu:%hu target-vcpu:%hu status:%d request:%s running:%s current:%s tsc:0x%lx",
			last_sgi_target->intid, last_sgi_target->source_vcpu_id,
			last_sgi_target->target_vcpu_id, last_sgi_target->status,
			shell_yes_no(last_sgi_target->request_pending),
			shell_yes_no(last_sgi_target->target_running),
			shell_yes_no(last_sgi_target->target_current),
			last_sgi_target->tsc);
		shell_item_line("          sgi:enabled:0x%04x pending:0x%04x active:0x%04x desc:%s/%s/%s/%s used_lrs:%u",
			(uint32_t)last_sgi_target->local_enabled,
			(uint32_t)last_sgi_target->local_pending,
			(uint32_t)last_sgi_target->local_active,
			shell_yes_no(last_sgi_target->desc_enabled),
			shell_yes_no(last_sgi_target->desc_pending),
			shell_yes_no(last_sgi_target->desc_active),
			shell_yes_no(last_sgi_target->desc_level),
			last_sgi_target->used_lrs);
		shell_item_line("          value:0x%016lx hcr:0x%016lx misr:0x%016lx lr0:0x%016lx lr1:0x%016lx",
			last_sgi_target->source_value, last_sgi_target->hcr,
			last_sgi_target->misr, last_sgi_target->lr0, last_sgi_target->lr1);
	}

	if (last_psci->tsc == 0UL) {
		shell_item_line(" psci:none");
	} else {
		shell_item_line(" psci:fn:0x%x source-vcpu:%hu target-vcpu:%hu ret:%ld tsc:0x%lx",
			last_psci->fn, last_psci->source_vcpu_id,
			last_psci->target_vcpu_id, last_psci->ret, last_psci->tsc);
		shell_item_line("      mpidr:0x%016lx entry:0x%016lx context:0x%016lx",
			last_psci->target_mpidr, last_psci->entry, last_psci->context);
	}

	if (last_cpuif->tsc == 0UL) {
		shell_item_line("cpuif-access:none");
	} else {
		/*
		 * The trapped ICC access is the guest-visible lifecycle command that
		 * can explain why a live LR stayed active, pending, or disabled.
		 */
		shell_item_line("cpuif-access:%s read:%s value:0x%016lx status:%d tsc:0x%lx",
			shell_cpuif_sysreg_to_str(last_cpuif->sysreg),
			shell_yes_no(last_cpuif->read), last_cpuif->value,
			last_cpuif->status, last_cpuif->tsc);
	}

	if (last_wfx->tsc == 0UL) {
		shell_item_line("  wfx:none");
	} else {
		shell_item_line("  wfx:type:%s pending:%s irq-masked:%s request:%s yield:%s tsc:0x%lx",
			last_wfx->is_wfe ? "wfe" : "wfi",
			shell_yes_no(last_wfx->pending_irq),
			shell_yes_no(last_wfx->irq_masked),
			shell_yes_no(last_wfx->request_pending),
			shell_yes_no(last_wfx->yielded), last_wfx->tsc);
		shell_item_line("      elr:0x%016lx esr:0x%016lx used_lrs:%u el2_masked:%s",
			last_wfx->elr, last_wfx->esr, last_wfx->used_lrs,
			shell_yes_no(last_wfx->cntv_el2_masked));
		shell_item_line("      cntv_ctl:0x%08x cntv_cval:0x%016lx cntvct:0x%016lx expired:%s",
			last_wfx->cntv_ctl, last_wfx->cntv_cval, last_wfx->cntvct,
			shell_yes_no(last_wfx->cntv_expired));
		shell_item_line("      hcr:0x%016lx misr:0x%016lx ap0r0:0x%016lx ap1r0:0x%016lx",
			last_wfx->hcr, last_wfx->misr, last_wfx->ap0r0, last_wfx->ap1r0);
		shell_item_line("      lr0:0x%016lx lr1:0x%016lx live_lr0:0x%016lx live_lr1:0x%016lx",
			last_wfx->lr0, last_wfx->lr1, last_wfx->live_lr0, last_wfx->live_lr1);
	}

	if (last_return->tsc == 0UL) {
		shell_item_line("return:none");
	} else {
		shell_item_line("return:source:%s elr:0x%016lx spsr:0x%016lx tsc:0x%lx",
			shell_exit_source_to_str(last_return->source),
			last_return->elr, last_return->spsr, last_return->tsc);
		shell_item_line("       cntv_ctl:0x%08x cntv_cval:0x%016lx cntvct:0x%016lx expired:%s",
			last_return->cntv_ctl, last_return->cntv_cval,
			last_return->cntvct, shell_yes_no(last_return->cntv_expired));
		shell_item_line("       hcr:0x%016lx vmcr:0x%016lx misr:0x%016lx eisr:0x%016lx elrsr:0x%016lx",
			last_return->hcr, last_return->vmcr, last_return->misr,
			last_return->eisr, last_return->elrsr);
		shell_item_line("       ap0r0:0x%016lx ap1r0:0x%016lx used_lrs:%u el2_masked:%s",
			last_return->ap0r0, last_return->ap1r0,
			last_return->used_lrs,
			shell_yes_no(last_return->cntv_el2_masked));
		shell_item_line("       sw_lr0:0x%016lx sw_lr1:0x%016lx live_lr0:0x%016lx live_lr1:0x%016lx",
			last_return->sw_lr0, last_return->sw_lr1,
			last_return->live_lr0, last_return->live_lr1);
		shell_item_line("       timer:virq:%u en:%s pend:%s act:%s level:%s host:%s/%s/%s valid:%s",
			last_return->timer_virq,
			shell_yes_no(last_return->timer_enabled),
			shell_yes_no(last_return->timer_pending),
			shell_yes_no(last_return->timer_active),
			shell_yes_no(last_return->timer_level),
			shell_yes_no(last_return->host_enabled),
			shell_yes_no(last_return->host_pending),
			shell_yes_no(last_return->host_active),
			shell_yes_no(last_return->host_valid));
	}

	shell_dumpstat_vgic_event("vgic-sync", &debug->last_vgic_sync);
	shell_dumpstat_vgic_event("vgic-maint", &debug->last_vgic_maintenance);
}

static void shell_dumpstat_vtimer_trace(const struct arm64_vcpu_vtimer_trace *trace)
{
	uint32_t count = trace->count;
	uint32_t start;
	uint32_t idx;

	if (count > ARM64_VCPU_VTIMER_TRACE_NUM) {
		count = ARM64_VCPU_VTIMER_TRACE_NUM;
	}
	if (count == 0U) {
		shell_item_line("trace:none");
		return;
	}

	start = (trace->head + ARM64_VCPU_VTIMER_TRACE_NUM - count) %
		ARM64_VCPU_VTIMER_TRACE_NUM;
	for (idx = 0U; idx < count; idx++) {
		uint32_t ring_idx = (start + idx) % ARM64_VCPU_VTIMER_TRACE_NUM;
		const struct arm64_vcpu_vtimer_trace_entry *entry = &trace->entry[ring_idx];
		int64_t delta = (int64_t)(entry->cval - entry->cntvct);

		shell_item_line("vt[%02u] %-7s pcpu:%hu virq:%u ctl:0x%08x exp:%3s mask:%3s p/a/l:%3s/%3s/%3s wr:%3s inj:%3s delta:%ld",
			idx, shell_vtimer_trace_event_to_str(entry->event),
			entry->pcpu_id, entry->virq, entry->ctl,
			shell_yes_no(entry->expired), shell_yes_no(entry->masked),
			shell_yes_no(entry->pending), shell_yes_no(entry->active),
			shell_yes_no(entry->level), shell_yes_no(entry->write),
			shell_yes_no(entry->injected), delta);
		shell_item_line("       cval:0x%016lx cntvct:0x%016lx lr0:0x%016lx hcr:0x%016lx misr:0x%016lx tsc:0x%lx",
			entry->cval, entry->cntvct, entry->lr0, entry->hcr,
			entry->misr, entry->tsc);
	}
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
		shell_item_line("[%02u] fp:0x%016lx  lr:0x%016lx  %s",
			idx, fp, lr, sym);
	} else {
		shell_item_line("[%02u] fp:0x%016lx  lr:0x%016lx",
			idx, fp, lr);
	}
}

static void shell_dumpstat_unwind_host_stack(uint64_t sp, uint64_t fp, uint64_t lr,
	uint64_t stack_start, uint64_t stack_end)
{
	uint32_t idx;

	shell_item_line("stack range:0x%016lx-0x%016lx", stack_start, stack_end);

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

	shell_item_line("pc:0x%016lx  sp:0x%016lx", regs->elr, regs->sp);

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
	struct arm64_vcpu_debug_info debug;
	struct arm64_vcpu_guest_ctx gctx;
	struct arm64_vgicv3_vcpu_ctx vgic_ctx;
	struct arm64_vgic_irq timer_irq;
	struct arm64_vgic_irq local_irq[DUMPSTAT_LOCAL_IRQ_NUM];
	struct arm64_gicv3_local_irq_state host_timer_irq;
	uint64_t live_cnthctl_el2;
	uint64_t live_cntvct_el0;
	uint64_t live_cntpct_el0;
	uint64_t live_cntvoff_el2;
	uint64_t live_cntv_cval_el0;
	uint64_t live_vbar_el1;
	uint64_t live_sp_el0;
	uint64_t live_elr_el1;
	uint64_t live_spsr_el1;
	uint64_t live_ich_hcr_el2;
	uint64_t live_ich_vmcr_el2;
	uint64_t live_ich_misr_el2;
	uint64_t live_ich_eisr_el2;
	uint64_t live_ich_elrsr_el2;
	uint64_t live_ich_ap0r0_el2;
	uint64_t live_ich_ap1r0_el2;
	uint64_t live_ich_lr[4U];
	uint64_t live_icc_pmr_el1;
	uint64_t live_icc_ctlr_el1;
	uint64_t live_icc_sre_el1;
	uint64_t live_icc_igrpen1_el1;
	uint64_t live_daif;
	uint64_t pending_req;
	uint64_t irqs_pending;
	uint64_t irqs_pending_mask;
	uint32_t live_cntv_ctl_el0;
	bool has_timer_irq;
	bool has_local_irq;
	bool has_live_timer;
	bool captured;
};

static void shell_dumpstat_capture(void *data)
{
	struct dumpstat_snapshot *snapshot = (struct dumpstat_snapshot *)data;

	if (get_running_vcpu(get_pcpu_id()) == snapshot->vcpu) {
		(void)memcpy_s(&snapshot->regs, sizeof(snapshot->regs),
			&snapshot->vcpu->arch.regs, sizeof(snapshot->regs));
		(void)memcpy_s(&snapshot->debug, sizeof(snapshot->debug),
			&snapshot->vcpu->arch.debug, sizeof(snapshot->debug));
		(void)memcpy_s(&snapshot->gctx, sizeof(snapshot->gctx),
			&snapshot->vcpu->arch.gctx, sizeof(snapshot->gctx));
		(void)memcpy_s(&snapshot->vgic_ctx, sizeof(snapshot->vgic_ctx),
			&snapshot->vcpu->arch.vgic, sizeof(snapshot->vgic_ctx));
		snapshot->pending_req = snapshot->vcpu->pending_req;
		snapshot->irqs_pending = snapshot->vcpu->arch.irqs_pending;
		snapshot->irqs_pending_mask = snapshot->vcpu->arch.irqs_pending_mask;
		if ((snapshot->vcpu->vm != NULL) &&
			(snapshot->vcpu->vcpu_id < ARM64_VGIC_MAX_VCPUS)) {
			(void)memcpy_s(snapshot->local_irq, sizeof(snapshot->local_irq),
				snapshot->vcpu->vm->arch_vm.vgic.irq[snapshot->vcpu->vcpu_id],
				sizeof(snapshot->local_irq));
			snapshot->has_local_irq = true;
		}
		snapshot->live_cnthctl_el2 = read_cnthctl_el2();
		snapshot->live_cntvct_el0 = read_cntvct_el0();
		snapshot->live_cntpct_el0 = read_cntpct_el0();
		snapshot->live_cntvoff_el2 = read_cntvoff_el2();
		snapshot->live_cntv_cval_el0 = read_cntv_cval_el0();
		snapshot->live_cntv_ctl_el0 = read_cntv_ctl_el0();
		/*
		 * EL1 exception state explains whether Linux will vector to the
		 * expected kernel table and whether EL1t stack state is drifting
		 * from the shadow context kept by the vCPU scheduler.
		 */
		snapshot->live_vbar_el1 = read_vbar_el1();
		snapshot->live_sp_el0 = read_sp_el0();
		snapshot->live_elr_el1 = read_elr_el1();
		snapshot->live_spsr_el1 = read_spsr_el1();
		snapshot->live_ich_hcr_el2 = read_ich_hcr_el2();
		snapshot->live_ich_vmcr_el2 = read_ich_vmcr_el2();
		snapshot->live_ich_misr_el2 = read_ich_misr_el2();
		snapshot->live_ich_eisr_el2 = read_ich_eisr_el2();
		snapshot->live_ich_elrsr_el2 = read_ich_elrsr_el2();
		snapshot->live_ich_ap0r0_el2 = read_ich_ap0r0_el2();
		snapshot->live_ich_ap1r0_el2 = read_ich_ap1r0_el2();
		/*
		 * Live LRs can differ from the saved software copy while the vCPU
		 * is running. Capturing both views identifies save/restore drift
		 * versus hardware state that is genuinely stuck at the CPU interface.
		 */
		snapshot->live_ich_lr[0U] = read_ich_lr_el2(0U);
		snapshot->live_ich_lr[1U] = read_ich_lr_el2(1U);
		snapshot->live_ich_lr[2U] = read_ich_lr_el2(2U);
		snapshot->live_ich_lr[3U] = read_ich_lr_el2(3U);
		/*
		 * The virtual CPU interface is gated by VMCR and guest ICC state,
		 * while Linux idle may hold DAIF.I set. Dumping both sides shows
		 * whether an LR is blocked by priority/group state or merely waiting
		 * for the guest to re-enable architectural IRQ handling.
		 */
		snapshot->live_icc_pmr_el1 = read_icc_pmr_el1();
		snapshot->live_icc_ctlr_el1 = read_icc_ctlr_el1();
		snapshot->live_icc_sre_el1 = read_icc_sre_el1();
		snapshot->live_icc_igrpen1_el1 = read_icc_igrpen1_el1();
		snapshot->live_daif = read_daif();
		arm64_gicv3_get_local_irq_state(get_pcpu_id(), DUMPSTAT_VTIMER_VIRQ,
			&snapshot->host_timer_irq);
		snapshot->has_live_timer = true;
		snapshot->captured = true;
	}
}

static const struct cpu_regs *shell_dumpstat_get_regs(struct acrn_vcpu *vcpu,
	struct dumpstat_snapshot *snapshot)
{
	uint16_t pcpu_id = vcpu->thread_obj.pcpu_id;

	(void)memset(snapshot, 0U, sizeof(*snapshot));
	snapshot->vcpu = vcpu;
	(void)memcpy_s(&snapshot->gctx, sizeof(snapshot->gctx),
		&vcpu->arch.gctx, sizeof(snapshot->gctx));
	(void)memcpy_s(&snapshot->vgic_ctx, sizeof(snapshot->vgic_ctx),
		&vcpu->arch.vgic, sizeof(snapshot->vgic_ctx));
	snapshot->pending_req = vcpu->pending_req;
	snapshot->irqs_pending = vcpu->arch.irqs_pending;
	snapshot->irqs_pending_mask = vcpu->arch.irqs_pending_mask;
	snapshot->has_timer_irq = false;
	if ((vcpu->vm != NULL) && (vcpu->vcpu_id < ARM64_VGIC_MAX_VCPUS)) {
		(void)memcpy_s(&snapshot->timer_irq, sizeof(snapshot->timer_irq),
			&vcpu->vm->arch_vm.vgic.irq[vcpu->vcpu_id][DUMPSTAT_VTIMER_VIRQ],
			sizeof(snapshot->timer_irq));
		(void)memcpy_s(snapshot->local_irq, sizeof(snapshot->local_irq),
			vcpu->vm->arch_vm.vgic.irq[vcpu->vcpu_id],
			sizeof(snapshot->local_irq));
		snapshot->has_timer_irq = true;
		snapshot->has_local_irq = true;
	}
	snapshot->has_live_timer = false;
	snapshot->captured = false;

	if ((vcpu->state == VCPU_RUNNING) &&
		(sched_get_current(pcpu_id) == &vcpu->thread_obj) &&
		(pcpu_id != get_pcpu_id())) {
		(void)smp_call_function_timeout(1UL << pcpu_id, shell_dumpstat_capture,
			snapshot, DUMPSTAT_SMP_CALL_TIMEOUT_US);
	}

	return snapshot->captured ? &snapshot->regs : &vcpu->arch.regs;
}

static void shell_dumpstat_el1_state(const struct dumpstat_snapshot *snapshot)
{
	const struct arm64_vcpu_guest_ctx *gctx = &snapshot->gctx;

	shell_item_line("shadow:vbar:0x%016lx sp_el0:0x%016lx",
		gctx->vbar_el1, gctx->sp_el0);
	shell_item_line("       elr_el1:0x%016lx spsr_el1:0x%016lx",
		gctx->elr_el1, gctx->spsr_el1);
	shell_item_line("       sctlr:0x%016lx tcr:0x%016lx cntkctl:0x%016lx",
		gctx->sctlr_el1, gctx->tcr_el1, gctx->cntkctl_el1);
	shell_item_line("       ttbr0:0x%016lx ttbr1:0x%016lx",
		gctx->ttbr0_el1, gctx->ttbr1_el1);
	if (snapshot->captured) {
		shell_item_line("  live:vbar:0x%016lx sp_el0:0x%016lx",
			snapshot->live_vbar_el1, snapshot->live_sp_el0);
		shell_item_line("       elr_el1:0x%016lx spsr_el1:0x%016lx",
			snapshot->live_elr_el1, snapshot->live_spsr_el1);
	} else {
		shell_item_line("  live:none");
	}
}

static void shell_dumpstat_timer_state(const struct dumpstat_snapshot *snapshot)
{
	const struct arm64_vcpu_guest_ctx *gctx = &snapshot->gctx;
	uint32_t idx;

	shell_item_line("shadow:virq:%u cntp_ctl:0x%08x cntp_cval:0x%016lx",
		gctx->timer_virq, gctx->cntp_ctl_el0, gctx->cntp_cval_el0);
	shell_item_line("       cntv_ctl:0x%08x cntv_cval:0x%016lx el2_masked:%s",
		gctx->cntv_ctl_el0, gctx->cntv_cval_el0,
		shell_yes_no(gctx->cntv_el2_masked));
	if (snapshot->has_live_timer) {
		uint64_t guest_now = snapshot->live_cntvct_el0;
		const struct arm64_gicv3_local_irq_state *host_irq = &snapshot->host_timer_irq;

		shell_item_line("  live:cnthctl:0x%016lx cntvct:0x%016lx",
			snapshot->live_cnthctl_el2, snapshot->live_cntvct_el0);
		shell_item_line("       cntpct:0x%016lx cntvoff:0x%016lx guest_now:0x%016lx",
			snapshot->live_cntpct_el0, snapshot->live_cntvoff_el2, guest_now);
		shell_item_line("       cntv_ctl:0x%08x cntv_cval:0x%016lx guest_delta:%ld",
			snapshot->live_cntv_ctl_el0, snapshot->live_cntv_cval_el0,
			(int64_t)(snapshot->live_cntv_cval_el0 - guest_now));
		if (host_irq->valid) {
			shell_item_line("       host_gic:intid:%u en:%s pend:%s act:%s group:%s prio:0x%02x",
				DUMPSTAT_VTIMER_VIRQ,
				shell_yes_no(host_irq->enabled != 0U),
				shell_yes_no(host_irq->pending != 0U),
				shell_yes_no(host_irq->active != 0U),
				shell_yes_no(host_irq->group != 0U),
				host_irq->priority);
		} else {
			shell_item_line("       host_gic:none");
		}
	} else {
		shell_item_line("  live:none");
	}

	if (snapshot->has_timer_irq) {
		const struct arm64_vgic_irq *irq = &snapshot->timer_irq;

		shell_item_line("  vgic:virq:%u enabled:%s pending:%s active:%s level:%s",
			irq->virq, shell_yes_no(irq->enabled), shell_yes_no(irq->pending),
			shell_yes_no(irq->active), shell_yes_no(irq->level));
	} else {
		shell_item_line("  vgic:none");
	}
	shell_item_line(" cpuif:vmcr:0x%016lx hcr:0x%016lx ctlr:0x%016lx",
		snapshot->vgic_ctx.vmcr, snapshot->vgic_ctx.hcr, snapshot->vgic_ctx.ctlr);
	shell_item_line("       sre:0x%016lx pmr:0x%016lx used_lrs:%u",
		snapshot->vgic_ctx.sre, snapshot->vgic_ctx.pmr, snapshot->vgic_ctx.used_lrs);
	if (snapshot->has_live_timer) {
		shell_item_line("       live_hcr:0x%016lx live_vmcr:0x%016lx misr:0x%016lx",
			snapshot->live_ich_hcr_el2, snapshot->live_ich_vmcr_el2,
			snapshot->live_ich_misr_el2);
		shell_item_line("       eisr:0x%016lx elrsr:0x%016lx",
			snapshot->live_ich_eisr_el2, snapshot->live_ich_elrsr_el2);
		shell_item_line("       live_ap0r0:0x%016lx live_ap1r0:0x%016lx",
			snapshot->live_ich_ap0r0_el2, snapshot->live_ich_ap1r0_el2);
		shell_item_line("       live_lr0:0x%016lx live_lr1:0x%016lx",
			snapshot->live_ich_lr[0U], snapshot->live_ich_lr[1U]);
		shell_item_line("       live_lr2:0x%016lx live_lr3:0x%016lx",
			snapshot->live_ich_lr[2U], snapshot->live_ich_lr[3U]);
		shell_item_line("       live_icc:pmr:0x%016lx ctlr:0x%016lx sre:0x%016lx igrpen1:0x%016lx",
			snapshot->live_icc_pmr_el1, snapshot->live_icc_ctlr_el1,
			snapshot->live_icc_sre_el1, snapshot->live_icc_igrpen1_el1);
		shell_item_line("       live_daif:0x%016lx", snapshot->live_daif);
	}
	for (idx = 0U; idx < snapshot->vgic_ctx.used_lrs; idx++) {
		shell_item_line("   lr%u:0x%016lx", idx, snapshot->vgic_ctx.lr[idx]);
	}
}

static void shell_dumpstat_local_irqs(const struct dumpstat_snapshot *snapshot)
{
	uint32_t idx;
	uint16_t sgi_enabled = 0U;
	uint16_t sgi_pending = 0U;
	uint16_t sgi_active = 0U;
	bool any = false;

	if (!snapshot->has_local_irq) {
		shell_item_line("local:none");
		return;
	}

	for (idx = 0U; idx < DUMPSTAT_LOCAL_IRQ_NUM; idx++) {
		const struct arm64_vgic_irq *irq = &snapshot->local_irq[idx];

		if (idx < ARM64_VGIC_SGI_NUM) {
			uint16_t bit = (uint16_t)(1UL << idx);

			if (irq->enabled) {
				sgi_enabled |= bit;
			}
			if (irq->pending) {
				sgi_pending |= bit;
			}
			if (irq->active) {
				sgi_active |= bit;
			}
		}
		if (irq->pending || irq->active || ((idx < ARM64_VGIC_SGI_NUM) && irq->enabled)) {
			shell_item_line("local:virq:%u enabled:%s pending:%s active:%s level:%s",
				irq->virq, shell_yes_no(irq->enabled),
				shell_yes_no(irq->pending), shell_yes_no(irq->active),
				shell_yes_no(irq->level));
			any = true;
		}
	}

	shell_item_line("local-sgi:enabled:0x%04x pending:0x%04x active:0x%04x",
		(uint32_t)sgi_enabled, (uint32_t)sgi_pending, (uint32_t)sgi_active);

	if (!any) {
		shell_item_line("local:none");
	}
}

static int32_t shell_dumpstat_vcpu(struct acrn_vcpu *vcpu)
{
	struct thread_object *current;
	struct dumpstat_snapshot snapshot;
	const struct cpu_regs *regs;
	const struct arm64_vcpu_debug_info *debug;

	current = (vcpu->state == VCPU_OFFLINE) ?
		NULL : sched_get_current(vcpu->thread_obj.pcpu_id);
	regs = shell_dumpstat_get_regs(vcpu, &snapshot);
	debug = snapshot.captured ? &snapshot.debug : &vcpu->arch.debug;

	shell_item_begin("vm%hu/vcpu%hu", vcpu->vm->vm_id, vcpu->vcpu_id);
	shell_item_line("pcpu:%hu sched:%s current:%s live:%s", vcpu->thread_obj.pcpu_id,
		vcpu_sched_state_to_str(vcpu),
		shell_yes_no(current == &vcpu->thread_obj),
		shell_yes_no(snapshot.captured));
	shell_item_line("requests:pending:0x%016lx arch-irqs:0x%016lx mask:0x%016lx",
		snapshot.pending_req, snapshot.irqs_pending,
		snapshot.irqs_pending_mask);
	shell_item_section("guest regs");
	shell_dumpstat_regs(regs);
	shell_item_section("recent events");
	shell_dumpstat_recent_events(debug);
	shell_item_section("el1 state");
	shell_dumpstat_el1_state(&snapshot);
	shell_item_section("timer/vgic state");
	shell_dumpstat_timer_state(&snapshot);
	shell_item_section("vtimer trace");
	shell_dumpstat_vtimer_trace(&debug->vtimer_trace);
	shell_item_section("local irq state");
	shell_dumpstat_local_irqs(&snapshot);
	if (vcpu->state != VCPU_OFFLINE) {
		shell_item_section("guest stack symbols:none");
		shell_dumpstat_vm_stack(vcpu, regs);
		shell_item_section("host stack symbols:sima");
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
