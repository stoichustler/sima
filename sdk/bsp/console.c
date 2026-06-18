/*
 * Copyright (C) 2018-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <cpu.h>
#include <pci.h>
#include <serial.h>
#include <shell.h>
#include <timer.h>
#include <ticks.h>
#include <vuart.h>
#include <logmsg.h>
#include <acrn_hv_defs.h>
#include <vm.h>
#include <console.h>
#include <boot.h>
#include <dbg_cmd.h>
#include <rtl.h>
#include <sprintf.h>
#include <spinlock.h>
#include <asm/guest/vm.h>

struct hv_timer console_timer;

#define CONSOLE_KICK_TIMER_TIMEOUT  10UL /* timeout is 10ms */
#ifndef CONFIG_VM_CONSOLE_RINGBUF_SIZE
#define CONFIG_VM_CONSOLE_RINGBUF_SIZE  4096U
#endif
#ifndef CONFIG_VM_CONSOLE_RINGBUF_VM_NUM
#define CONFIG_VM_CONSOLE_RINGBUF_VM_NUM CONFIG_MAX_VM_NUM
#endif
#if (CONFIG_VM_CONSOLE_RINGBUF_SIZE < 2U)
#error "CONFIG_VM_CONSOLE_RINGBUF_SIZE must be at least 2 bytes"
#endif
#if ((CONFIG_VM_CONSOLE_RINGBUF_SIZE & (CONFIG_VM_CONSOLE_RINGBUF_SIZE - 1U)) != 0U)
#error "CONFIG_VM_CONSOLE_RINGBUF_SIZE must be a power of two"
#endif
#define VM_CONSOLE_RINGBUF_MASK     (CONFIG_VM_CONSOLE_RINGBUF_SIZE - 1U)
#define VM_CONSOLE_RINGBUF_CAPACITY (CONFIG_VM_CONSOLE_RINGBUF_SIZE - 1U)
#ifndef CONFIG_VM_CONSOLE_DRAIN_BUDGET
#define CONFIG_VM_CONSOLE_DRAIN_BUDGET 128U
#endif
#define VM_CONSOLE_DRAIN_BUDGET CONFIG_VM_CONSOLE_DRAIN_BUDGET
#define VM_CONSOLE_DRAIN_BUF_SIZE 1024U
#define VM_CONSOLE_PREFIX_MAX_SIZE 16U
#define VM_CONSOLE_EXCEPTION_RINGBUF_SIZE 4096U
#define VM_CONSOLE_EXCEPTION_RINGBUF_MASK (VM_CONSOLE_EXCEPTION_RINGBUF_SIZE - 1U)
#define VM_CONSOLE_EXCEPTION_RINGBUF_CAPACITY (VM_CONSOLE_EXCEPTION_RINGBUF_SIZE - 1U)
/* Switching key combinations for shell and uart console */
#define GUEST_CONSOLE_TO_HV_SWITCH_KEY  0x4U /* Ctrl-D */
uint16_t console_vmid = CONFIG_CONSOLE_DEFAULT_VM;

uint16_t console_loglevel = CONFIG_CONSOLE_LOGLEVEL_DEFAULT;
static spinlock_t console_log_lock;

struct vm_console_ringbuf {
	spinlock_t lock;
	uint32_t cons;
	uint32_t prod;
	uint32_t high_water;
	uint64_t input_bytes;
	uint64_t stored_bytes;
	uint64_t drained_bytes;
	uint64_t dropped_bytes;
	uint64_t overflow_events;
	uint64_t last_overflow_tsc;
	bool pending;
	bool draining;
	bool line_start;
	bool last_cr;
	char buf[CONFIG_VM_CONSOLE_RINGBUF_SIZE];
};

struct vm_exception_ringbuf {
	spinlock_t lock;
	uint32_t cons;
	uint32_t prod;
	uint32_t high_water;
	uint64_t input_bytes;
	uint64_t stored_bytes;
	uint64_t drained_bytes;
	uint64_t dropped_bytes;
	uint64_t overflow_events;
	uint64_t last_overflow_tsc;
	char buf[VM_CONSOLE_EXCEPTION_RINGBUF_SIZE];
};

static struct vm_console_ringbuf vm_console_ringbufs[CONFIG_VM_CONSOLE_RINGBUF_VM_NUM];
static struct vm_exception_ringbuf vm_exception_ringbufs[CONFIG_VM_CONSOLE_RINGBUF_VM_NUM];
static char vm_console_drain_buf[VM_CONSOLE_DRAIN_BUF_SIZE];
static char vm_console_output_buf[VM_CONSOLE_DRAIN_BUF_SIZE + 128U];

static void console_vm_ring_put(uint16_t vmid, char ch);
static void console_vm_ring_drain_internal(uint16_t vmid);

void console_init(void)
{
	/*
	 * Enable UART as early as possible.
	 * Then we could use printf for debugging on early boot stage.
	 */
	serial_init(true);

	spinlock_init(&console_log_lock);
	for (uint16_t i = 0U; i < CONFIG_VM_CONSOLE_RINGBUF_VM_NUM; i++) {
		spinlock_init(&vm_console_ringbufs[i].lock);
		vm_console_ringbufs[i].line_start = true;
		spinlock_init(&vm_exception_ringbufs[i].lock);
	}
}

void console_putc(const char *ch)
{
	(void)serial_puts(ch, 1U);
}

bool console_is_hv(void)
{
	return (console_vmid == ACRN_INVALID_VMID);
}

bool console_is_vm_active(uint16_t vmid)
{
	return console_vmid == vmid;
}

bool console_vm_tx_put(uint16_t vmid, char ch)
{
	bool handled = false;

	if (vmid < CONFIG_VM_CONSOLE_RINGBUF_VM_NUM) {
		console_vm_ring_put(vmid, ch);
		handled = true;
	}

	return handled;
}

size_t console_write(const char *s, size_t len)
{
	return  serial_puts(s, len);
}

char console_getc(void)
{
	return serial_getc();
}

static void console_vm_ring_put(uint16_t vmid, char ch)
{
	struct vm_console_ringbuf *rb;
	uint64_t rflags;
	uint32_t prod;
	uint32_t queued;

	if (vmid < CONFIG_VM_CONSOLE_RINGBUF_VM_NUM) {
		rb = &vm_console_ringbufs[vmid];
		spinlock_irqsave_obtain(&rb->lock, &rflags);
		rb->input_bytes++;
		prod = rb->prod;
		rb->buf[prod & VM_CONSOLE_RINGBUF_MASK] = ch;
		rb->prod = prod + 1U;
		if ((rb->prod - rb->cons) > VM_CONSOLE_RINGBUF_CAPACITY) {
			rb->dropped_bytes += (rb->prod - rb->cons) - VM_CONSOLE_RINGBUF_CAPACITY;
			rb->overflow_events++;
			rb->last_overflow_tsc = cpu_ticks();
			rb->cons = rb->prod - VM_CONSOLE_RINGBUF_CAPACITY;
		}
		queued = rb->prod - rb->cons;
		if (queued > rb->high_water) {
			rb->high_water = queued;
		}
		rb->stored_bytes++;
		rb->pending = true;
		spinlock_irqrestore_release(&rb->lock, rflags);
	}
}

void console_vm_ring_drain(uint16_t vmid)
{
	console_vm_ring_drain_internal(vmid);
}

static uint32_t console_ring_queued(uint32_t prod, uint32_t cons, uint32_t capacity)
{
	uint32_t queued = prod - cons;

	return (queued > capacity) ? capacity : queued;
}

bool console_vm_ring_get_stats(uint16_t vmid, struct console_vm_ring_stats *stats)
{
	struct vm_console_ringbuf *rb;
	uint64_t rflags;
	bool valid = false;

	if ((stats != NULL) && (vmid < CONFIG_VM_CONSOLE_RINGBUF_VM_NUM)) {
		rb = &vm_console_ringbufs[vmid];
		spinlock_irqsave_obtain(&rb->lock, &rflags);
		stats->vmid = vmid;
		stats->size = CONFIG_VM_CONSOLE_RINGBUF_SIZE;
		stats->capacity = VM_CONSOLE_RINGBUF_CAPACITY;
		stats->drain_budget = VM_CONSOLE_DRAIN_BUDGET;
		stats->queued = console_ring_queued(rb->prod, rb->cons, VM_CONSOLE_RINGBUF_CAPACITY);
		stats->high_water = rb->high_water;
		stats->input_bytes = rb->input_bytes;
		stats->stored_bytes = rb->stored_bytes;
		stats->drained_bytes = rb->drained_bytes;
		stats->dropped_bytes = rb->dropped_bytes;
		stats->overflow_events = rb->overflow_events;
		stats->last_overflow_tsc = rb->last_overflow_tsc;
		stats->pending = rb->pending;
		stats->draining = rb->draining;
		spinlock_irqrestore_release(&rb->lock, rflags);
		valid = true;
	}

	return valid;
}

void console_vm_exception_log(uint16_t vmid, const char *buf, size_t len)
{
	struct vm_exception_ringbuf *rb;
	uint64_t rflags;
	uint32_t prod;
	uint32_t queued;

	if ((buf != NULL) && (vmid < CONFIG_VM_CONSOLE_RINGBUF_VM_NUM)) {
		rb = &vm_exception_ringbufs[vmid];
		spinlock_irqsave_obtain(&rb->lock, &rflags);
		for (size_t i = 0U; i < len; i++) {
			rb->input_bytes++;
			prod = rb->prod;
			rb->buf[prod & VM_CONSOLE_EXCEPTION_RINGBUF_MASK] = buf[i];
			rb->prod = prod + 1U;
			if ((rb->prod - rb->cons) > VM_CONSOLE_EXCEPTION_RINGBUF_CAPACITY) {
				rb->dropped_bytes += (rb->prod - rb->cons) -
					VM_CONSOLE_EXCEPTION_RINGBUF_CAPACITY;
				rb->overflow_events++;
				rb->last_overflow_tsc = cpu_ticks();
				rb->cons = rb->prod - VM_CONSOLE_EXCEPTION_RINGBUF_CAPACITY;
			}
			queued = rb->prod - rb->cons;
			if (queued > rb->high_water) {
				rb->high_water = queued;
			}
			rb->stored_bytes++;
		}
		spinlock_irqrestore_release(&rb->lock, rflags);
	}
}

uint32_t console_vm_exception_count(void)
{
	return CONFIG_VM_CONSOLE_RINGBUF_VM_NUM;
}

bool console_vm_exception_get_stats(uint16_t vmid, struct console_vm_exception_stats *stats)
{
	struct vm_exception_ringbuf *rb;
	uint64_t rflags;
	bool valid = false;

	if ((stats != NULL) && (vmid < CONFIG_VM_CONSOLE_RINGBUF_VM_NUM)) {
		rb = &vm_exception_ringbufs[vmid];
		spinlock_irqsave_obtain(&rb->lock, &rflags);
		stats->vmid = vmid;
		stats->size = VM_CONSOLE_EXCEPTION_RINGBUF_SIZE;
		stats->capacity = VM_CONSOLE_EXCEPTION_RINGBUF_CAPACITY;
		stats->queued = console_ring_queued(rb->prod, rb->cons,
			VM_CONSOLE_EXCEPTION_RINGBUF_CAPACITY);
		stats->high_water = rb->high_water;
		stats->input_bytes = rb->input_bytes;
		stats->stored_bytes = rb->stored_bytes;
		stats->dropped_bytes = rb->dropped_bytes;
		stats->overflow_events = rb->overflow_events;
		stats->last_overflow_tsc = rb->last_overflow_tsc;
		stats->pending = (rb->prod != rb->cons);
		spinlock_irqrestore_release(&rb->lock, rflags);
		valid = true;
	}

	return valid;
}

uint32_t console_vm_exception_copy(uint16_t vmid, uint32_t offset, char *buf, uint32_t len)
{
	struct vm_exception_ringbuf *rb;
	uint64_t rflags;
	uint32_t queued;
	uint32_t count = 0U;
	uint32_t cons;
	uint32_t first;

	if ((buf != NULL) && (vmid < CONFIG_VM_CONSOLE_RINGBUF_VM_NUM)) {
		rb = &vm_exception_ringbufs[vmid];
		spinlock_irqsave_obtain(&rb->lock, &rflags);
		queued = console_ring_queued(rb->prod, rb->cons, VM_CONSOLE_EXCEPTION_RINGBUF_CAPACITY);
		if (offset < queued) {
			count = queued - offset;
			if (count > len) {
				count = len;
			}
			cons = rb->cons + offset;
			first = VM_CONSOLE_EXCEPTION_RINGBUF_SIZE -
				(cons & VM_CONSOLE_EXCEPTION_RINGBUF_MASK);
			if (first > count) {
				first = count;
			}
			(void)memcpy(buf, &rb->buf[cons & VM_CONSOLE_EXCEPTION_RINGBUF_MASK], first);
			if (first < count) {
				(void)memcpy(&buf[first], rb->buf, count - first);
			}
		}
		spinlock_irqrestore_release(&rb->lock, rflags);
	}

	return count;
}

void console_vm_exception_clear(uint16_t vmid)
{
	struct vm_exception_ringbuf *rb;
	uint64_t rflags;

	if (vmid < CONFIG_VM_CONSOLE_RINGBUF_VM_NUM) {
		rb = &vm_exception_ringbufs[vmid];
		spinlock_irqsave_obtain(&rb->lock, &rflags);
		rb->cons = 0U;
		rb->prod = 0U;
		rb->high_water = 0U;
		rb->input_bytes = 0UL;
		rb->stored_bytes = 0UL;
		rb->drained_bytes = 0UL;
		rb->dropped_bytes = 0UL;
		rb->overflow_events = 0UL;
		rb->last_overflow_tsc = 0UL;
		spinlock_irqrestore_release(&rb->lock, rflags);
	}
}

void console_vm_exception_clear_all(void)
{
	for (uint16_t vmid = 0U; vmid < CONFIG_VM_CONSOLE_RINGBUF_VM_NUM; vmid++) {
		console_vm_exception_clear(vmid);
	}
}

/*
 * @post return != NULL
 */
struct acrn_vuart *vm_console_vuart(struct acrn_vm *vm)
{
	return &vm->vuart[0];
}

static void vuart_console_rx_chars(struct acrn_vuart *vu)
{
	uint16_t target_vmid = console_vmid;
	char ch = -1;
	bool recv = false;

	while (1) {
		/* Get data from physical uart */
		ch = serial_getc();
		if (ch == -1)
			break;

		if (ch == GUEST_CONSOLE_TO_HV_SWITCH_KEY) {
			console_vm_ring_drain_internal(target_vmid);
			console_vmid = ACRN_INVALID_VMID;
			printf("\r\n\r\n──────── [switch to BEAU shell] ────────\r\n");
			goto exit;
		} else if (vu != NULL) {
			vuart_putchar(vu, ch);
			recv = true;
		}
	}

exit:
	if (recv && (vu != NULL) && (console_vmid == target_vmid)) {
		vuart_notify_rx(vu);
	}
}

static struct acrn_vuart *vuart_console_active(void)
{
	struct acrn_vm *vm = NULL;
	struct acrn_vuart *vu = NULL;

	if (console_vmid < CONFIG_MAX_VM_NUM) {
		vm = get_vm_from_vmid(console_vmid);
		if (!is_paused_vm(vm) && !is_poweroff_vm(vm)) {
			vu = vm_console_vuart(vm);
		} else {
			/* Console vm is invalid, switch back to HV-Shell */
			console_vmid = ACRN_INVALID_VMID;
		}
	}

	return ((vu != NULL) && vu->active) ? vu : NULL;
}

static bool console_vm_ring_drain_begin(struct vm_console_ringbuf *rb)
{
	uint64_t rflags;
	bool draining;

	spinlock_irqsave_obtain(&rb->lock, &rflags);
	draining = rb->draining;
	if (!draining) {
		rb->draining = true;
	}
	spinlock_irqrestore_release(&rb->lock, rflags);

	return !draining;
}

static void console_vm_ring_drain_end(struct vm_console_ringbuf *rb)
{
	uint64_t rflags;

	spinlock_irqsave_obtain(&rb->lock, &rflags);
	rb->draining = false;
	spinlock_irqrestore_release(&rb->lock, rflags);
}

static uint32_t console_vm_ring_claim(struct vm_console_ringbuf *rb, char *buf, uint32_t len)
{
	uint64_t rflags;
	uint32_t count;
	uint32_t cons;
	uint32_t first;

	spinlock_irqsave_obtain(&rb->lock, &rflags);
	count = rb->prod - rb->cons;
	if (count > VM_CONSOLE_RINGBUF_CAPACITY) {
		rb->cons = rb->prod - VM_CONSOLE_RINGBUF_CAPACITY;
		count = VM_CONSOLE_RINGBUF_CAPACITY;
	}
	if (count > len) {
		count = len;
	}
	if (count > 0U) {
		cons = rb->cons;
		first = CONFIG_VM_CONSOLE_RINGBUF_SIZE - (cons & VM_CONSOLE_RINGBUF_MASK);
		if (first > count) {
			first = count;
		}
		(void)memcpy(buf, &rb->buf[cons & VM_CONSOLE_RINGBUF_MASK], first);
		if (first < count) {
			(void)memcpy(&buf[first], rb->buf, count - first);
		}
		rb->cons = cons + count;
		rb->drained_bytes += count;
	}
	rb->pending = (rb->prod != rb->cons);
	spinlock_irqrestore_release(&rb->lock, rflags);

	return count;
}

static void console_vm_prefixed_write_flush(char *out, uint32_t *out_len)
{
	if (*out_len > 0U) {
		(void)console_write(out, *out_len);
		*out_len = 0U;
	}
}

static void console_vm_prefixed_write_byte(char *out, uint32_t *out_len, char ch)
{
	if (*out_len == ARRAY_SIZE(vm_console_output_buf)) {
		console_vm_prefixed_write_flush(out, out_len);
	}
	out[*out_len] = ch;
	(*out_len)++;
}

static void console_vm_prefixed_write_bytes(char *out, uint32_t *out_len,
	const char *buf, uint32_t len)
{
	for (uint32_t idx = 0U; idx < len; idx++) {
		console_vm_prefixed_write_byte(out, out_len, buf[idx]);
	}
}

static void console_vm_ring_write_prefixed(uint16_t vmid, struct vm_console_ringbuf *rb,
	const char *buf, uint32_t len)
{
	char prefix[VM_CONSOLE_PREFIX_MAX_SIZE];
	uint32_t out_len = 0U;
	size_t prefix_len;

	(void)snprintf(prefix, sizeof(prefix), "[vmid %u] ", vmid);
	prefix_len = strnlen_s(prefix, sizeof(prefix));

	for (uint32_t idx = 0U; idx < len; idx++) {
		char ch = buf[idx];

		if (rb->line_start) {
			if (!rb->last_cr || (ch != '\n')) {
				console_vm_prefixed_write_bytes(vm_console_output_buf, &out_len,
					prefix, (uint32_t)prefix_len);
				rb->line_start = false;
			}
		}

		console_vm_prefixed_write_byte(vm_console_output_buf, &out_len, ch);
		if (ch == '\r') {
			rb->line_start = true;
			rb->last_cr = true;
		} else if (ch == '\n') {
			rb->line_start = true;
			rb->last_cr = false;
		} else {
			rb->last_cr = false;
		}
	}

	console_vm_prefixed_write_flush(vm_console_output_buf, &out_len);
}

static void console_vm_ring_drain_internal(uint16_t vmid)
{
	struct vm_console_ringbuf *rb;
	uint32_t count;
	uint32_t budget;
	uint32_t chunk;

	if (vmid < CONFIG_VM_CONSOLE_RINGBUF_VM_NUM) {
		rb = &vm_console_ringbufs[vmid];
		if (console_vm_ring_drain_begin(rb)) {
			/*
			 * Host serial output is synchronous. Keep each timer callback
			 * short so selecting a chatty Linux VM does not monopolize the
			 * BEAU shell pCPU while a whole boot-log ring is replayed at once.
			 */
			budget = VM_CONSOLE_DRAIN_BUDGET;
			do {
				chunk = (budget < VM_CONSOLE_DRAIN_BUF_SIZE) ? budget : VM_CONSOLE_DRAIN_BUF_SIZE;
				if (chunk == 0U) {
					break;
				}
				count = console_vm_ring_claim(rb, vm_console_drain_buf, chunk);
				if (count > 0U) {
					console_vm_ring_write_prefixed(vmid, rb, vm_console_drain_buf, count);
					budget -= count;
				}
			} while ((count == chunk) && (budget > 0U));
			console_vm_ring_drain_end(rb);
		}
	}
}

bool console_vm_kick(void)
{
	struct acrn_vuart *vu;
	bool handled = !console_is_hv();

	if (handled) {
		vu = vuart_console_active();
		vuart_console_rx_chars(vu);

		if (vu != NULL) {
			console_vm_ring_drain_internal(console_vmid);
		}
	}

	return handled;
}

static void console_timer_callback(__unused void *data)
{
	if (!console_vm_kick()) {
		shell_kick();
	}
}

void console_setup_timer(void)
{
	uint64_t period_in_cycle, fire_tsc;

	period_in_cycle = TICKS_PER_MS * CONSOLE_KICK_TIMER_TIMEOUT;
	fire_tsc = cpu_ticks() + period_in_cycle;
	initialize_timer(&console_timer,
			console_timer_callback, NULL,
			fire_tsc, period_in_cycle);

	/* Start an periodic timer */
	if (add_timer(&console_timer) != 0) {
		pr_err("failed to add console kick timer");
	}
}

/* When lapic-pt is enabled for a vcpu working on the pcpu hosting
 * console timer, we utilize vm-exits to drive the console.
 *
 * Note that currently this approach will result in a laggy shell when
 * the number of VM-exits/second is low (which is mostly true when lapic-pt is
 * enabled).
 */
void console_vmexit_callback(struct acrn_vcpu *vcpu)
{
	static uint64_t prev_tsc = 0;
	uint64_t tsc;

	if (pcpuid_from_vcpu(vcpu) == VUART_TIMER_CPU) {
		tsc = cpu_ticks();
		if (tsc - prev_tsc > (TICKS_PER_MS * CONSOLE_KICK_TIMER_TIMEOUT)) {
			console_timer_callback(NULL);
			prev_tsc = tsc;
		}
	}
}

void suspend_console(void)
{
	if (VUART_TIMER_CPU == BSP_CPU_ID) {
		del_timer(&console_timer);
	}
}

void resume_console(void)
{
	if (VUART_TIMER_CPU == BSP_CPU_ID) {
		console_setup_timer();
	}
}

bool console_need_log(uint32_t severity)
{
	return (severity <= console_loglevel);
}

void console_log(char *buffer)
{
	uint64_t rflags;
	size_t len;

	spinlock_irqsave_obtain(&console_log_lock ,&rflags);

	len = strnlen_s(buffer, LOG_MESSAGE_MAX_SIZE);
	while ((len > 0U) && ((buffer[len - 1U] == '\n') || (buffer[len - 1U] == '\r'))) {
		len--;
	}

	/* Send buffer to stdout with one normalized line ending. */
	(void)console_write(buffer, len);
	printf("\r\n");

	spinlock_irqrestore_release(&console_log_lock, rflags);
}
