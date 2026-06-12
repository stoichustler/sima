/*
 * Copyright (C) 2026 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <vm.h>
#include <vcpu.h>
#include <io_req.h>
#include <console.h>
#include <vuart.h>
#include <debug/serial.h>
#include <asm/platform.h>
#include <asm/guest/vpl011.h>

#define PL011_DR		0x000U
#define PL011_FR		0x018U
#define PL011_IBRD		0x024U
#define PL011_FBRD		0x028U
#define PL011_LCRH		0x02CU
#define PL011_CR		0x030U
#define PL011_IFLS		0x034U
#define PL011_IMSC		0x038U
#define PL011_RIS		0x03CU
#define PL011_MIS		0x040U
#define PL011_ICR		0x044U

#define PL011_FR_TXFE		(1U << 7U)
#define PL011_FR_RXFE		(1U << 4U)
#define PL011_INT_TX		(1U << 5U)
#define PL011_INT_RX		(1U << 4U)

#define PL011_PID_BASE		0xFE0U
#define PL011_CID_BASE		0xFF0U

static const uint8_t pl011_pid[] = { 0x11U, 0x10U, 0x14U, 0x00U, 0x0dU, 0xf0U, 0x05U, 0xb1U };
static const uint8_t pl011_cid[] = { 0x0dU, 0xf0U, 0x05U, 0xb1U };

struct arm64_vpl011 {
	uint32_t ibrd;
	uint32_t fbrd;
	uint32_t lcrh;
	uint32_t cr;
	uint32_t ifls;
	uint32_t imsc;
	uint32_t ris;
};

static struct arm64_vpl011 vpl011_devs[CONFIG_MAX_VM_NUM];

static uint32_t vpl011_rx_int_state(struct acrn_vm *vm)
{
	return vuart_rx_pending(vm_console_vuart(vm)) ? PL011_INT_RX : 0U;
}

static void vpl011_update_irq(struct acrn_vm *vm, struct arm64_vpl011 *vu)
{
	uint32_t rx_state = vpl011_rx_int_state(vm);
	uint32_t pending = vu->ris | rx_state;
	bool assert = ((pending & vu->imsc) != 0U);

	arch_trigger_level_intr(vm, arm64_platform_guest_uart_irq(vm->vm_id), assert);
}

static void vpl011_notify_rx(struct acrn_vuart *console)
{
	struct acrn_vm *vm = console->vm;
	struct arm64_vpl011 *vu = &vpl011_devs[vm->vm_id];

	if (vuart_rx_pending(console)) {
		console->ier |= IER_ERBFI;
	} else {
		console->ier &= (uint8_t)~IER_ERBFI;
	}
	vpl011_update_irq(vm, vu);
}

static const struct vuart_backend_ops vpl011_backend_ops = {
	.notify_rx = vpl011_notify_rx,
};

void arm64_vpl011_init_vm(struct acrn_vm *vm)
{
	struct arm64_vpl011 *vu = &vpl011_devs[vm->vm_id];
	struct acrn_vuart *console;

	(void)memset(vu, 0U, sizeof(*vu));
	vu->ifls = 0x12U;
	vu->cr = (1U << 0U) | (1U << 8U) | (1U << 9U);
	init_console_vuart(vm, arm64_platform_guest_uart_irq(vm->vm_id));
	console = vm_console_vuart(vm);
	vuart_set_backend(console, &vpl011_backend_ops);
}

static uint32_t vpl011_read(struct acrn_vm *vm, struct arm64_vpl011 *vu, uint32_t offset)
{
	struct acrn_vuart *console = vm_console_vuart(vm);
	uint32_t value = 0U;
	char ch;

	switch (offset) {
	case PL011_DR:
		ch = vuart_get_rx_char(console);
		if (ch != -1) {
			value = (uint32_t)(uint8_t)ch;
		}
		if (!vuart_rx_pending(console)) {
			console->ier &= (uint8_t)~IER_ERBFI;
		}
		break;
	case PL011_FR:
		value = PL011_FR_TXFE;
		if (!vuart_rx_pending(console)) {
			value |= PL011_FR_RXFE;
		}
		break;
	case PL011_IBRD:
		value = vu->ibrd;
		break;
	case PL011_FBRD:
		value = vu->fbrd;
		break;
	case PL011_LCRH:
		value = vu->lcrh;
		break;
	case PL011_CR:
		value = vu->cr;
		break;
	case PL011_IFLS:
		value = vu->ifls;
		break;
	case PL011_IMSC:
		value = vu->imsc;
		break;
	case PL011_RIS:
		value = vu->ris | vpl011_rx_int_state(vm);
		break;
	case PL011_MIS:
		value = (vu->ris | vpl011_rx_int_state(vm)) & vu->imsc;
		break;
	default:
		if ((offset >= PL011_PID_BASE) && (offset < (PL011_PID_BASE + sizeof(pl011_pid) * 4U))) {
			value = pl011_pid[(offset - PL011_PID_BASE) >> 2U];
		} else if ((offset >= PL011_CID_BASE) && (offset < (PL011_CID_BASE + sizeof(pl011_cid) * 4U))) {
			value = pl011_cid[(offset - PL011_CID_BASE) >> 2U];
		}
		break;
	}

	vpl011_update_irq(vm, vu);
	return value;
}

static void vpl011_write(struct acrn_vm *vm, struct arm64_vpl011 *vu, uint32_t offset, uint32_t value)
{
	struct acrn_vuart *console = vm_console_vuart(vm);
	bool update_irq = true;
	char ch;

	switch (offset) {
	case PL011_DR:
		ch = (char)(value & 0xffU);
		(void)console_vm_tx_put(vm->vm_id, ch);
		if ((vu->ris & PL011_INT_TX) == 0U) {
			vu->ris |= PL011_INT_TX;
		} else {
			update_irq = false;
		}
		break;
	case PL011_IBRD:
		vu->ibrd = value;
		break;
	case PL011_FBRD:
		vu->fbrd = value;
		break;
	case PL011_LCRH:
		vu->lcrh = value;
		break;
	case PL011_CR:
		vu->cr = value;
		break;
	case PL011_IFLS:
		vu->ifls = value;
		break;
	case PL011_IMSC:
		vu->imsc = value;
		if ((value & PL011_INT_RX) != 0U) {
			console->ier |= IER_ERBFI;
		} else {
			console->ier &= (uint8_t)~IER_ERBFI;
		}
		break;
	case PL011_ICR:
		vu->ris &= ~value;
		break;
	default:
		break;
	}

	if (update_irq) {
		vpl011_update_irq(vm, vu);
	}
}

int32_t arm64_vpl011_mmio_handler(struct io_request *io_req, void *handler_private_data)
{
	struct acrn_vm *vm = (struct acrn_vm *)handler_private_data;
	struct arm64_vpl011 *vu;
	struct acrn_mmio_request *mmio = &io_req->reqs.mmio_request;
	uint64_t base;
	uint32_t offset;
	int32_t ret = -EINVAL;

	if ((vm != NULL) && (vm->vm_id < CONFIG_MAX_VM_NUM) &&
		((mmio->size == 1UL) || (mmio->size == 2UL) || (mmio->size == 4UL))) {
		base = arm64_platform_guest_uart_base(vm->vm_id);
		offset = (uint32_t)(mmio->address - base);
		vu = &vpl011_devs[vm->vm_id];

		if (mmio->direction == ACRN_IOREQ_DIR_READ) {
			mmio->value = vpl011_read(vm, vu, offset);
		} else {
			vpl011_write(vm, vu, offset, (uint32_t)mmio->value);
		}
		ret = 0;
	}

	return ret;
}
