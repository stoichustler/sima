/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_CPU_H
#define ARM64_CPU_H

#include <asm/offset.h>

#ifndef ASSEMBLER
#include <types.h>
#include <lib/util.h>
#include <logmsg.h>
#ifndef CONFIG_STATIC_ARM64_PLATFORM
#include <board_info.h>
#endif
#include <barrier.h>
#endif

#define DAIF_IRQ		(1UL << 7U)
#define DAIF_FIQ		(1UL << 6U)
#define DAIF_ASYNC_ABORT	(1UL << 8U)
#define DAIF_DEBUG		(1UL << 9U)
#define DAIF_ALL		(DAIF_DEBUG | DAIF_ASYNC_ABORT | DAIF_IRQ | DAIF_FIQ)

#define CURRENTEL_EL2		0x8

#ifndef ASSEMBLER

struct cpu_regs {
	uint64_t x0;
	uint64_t x1;
	uint64_t x2;
	uint64_t x3;
	uint64_t x4;
	uint64_t x5;
	uint64_t x6;
	uint64_t x7;
	uint64_t x8;
	uint64_t x9;
	uint64_t x10;
	uint64_t x11;
	uint64_t x12;
	uint64_t x13;
	uint64_t x14;
	uint64_t x15;
	uint64_t x16;
	uint64_t x17;
	uint64_t x18;
	uint64_t x19;
	uint64_t x20;
	uint64_t x21;
	uint64_t x22;
	uint64_t x23;
	uint64_t x24;
	uint64_t x25;
	uint64_t x26;
	uint64_t x27;
	uint64_t x28;
	uint64_t x29;
	uint64_t lr;
	uint64_t sp;
	uint64_t elr;
	uint64_t spsr;
	uint64_t esr;
	uint64_t far;
	uint64_t hpfar;
	uint64_t host_tpidr;
	uint64_t exc_sp;
	uint64_t reserved;
};

_Static_assert(offsetof(struct cpu_regs, x0) == CPU_REGS_OFFSET_X0, "cpu_regs.x0 offset mismatch");
_Static_assert(offsetof(struct cpu_regs, x1) == CPU_REGS_OFFSET_X1, "cpu_regs.x1 offset mismatch");
_Static_assert(offsetof(struct cpu_regs, x2) == CPU_REGS_OFFSET_X2, "cpu_regs.x2 offset mismatch");
_Static_assert(offsetof(struct cpu_regs, x3) == CPU_REGS_OFFSET_X3, "cpu_regs.x3 offset mismatch");
_Static_assert(offsetof(struct cpu_regs, x4) == CPU_REGS_OFFSET_X4, "cpu_regs.x4 offset mismatch");
_Static_assert(offsetof(struct cpu_regs, x5) == CPU_REGS_OFFSET_X5, "cpu_regs.x5 offset mismatch");
_Static_assert(offsetof(struct cpu_regs, x6) == CPU_REGS_OFFSET_X6, "cpu_regs.x6 offset mismatch");
_Static_assert(offsetof(struct cpu_regs, x7) == CPU_REGS_OFFSET_X7, "cpu_regs.x7 offset mismatch");
_Static_assert(offsetof(struct cpu_regs, x8) == CPU_REGS_OFFSET_X8, "cpu_regs.x8 offset mismatch");
_Static_assert(offsetof(struct cpu_regs, x9) == CPU_REGS_OFFSET_X9, "cpu_regs.x9 offset mismatch");
_Static_assert(offsetof(struct cpu_regs, x10) == CPU_REGS_OFFSET_X10, "cpu_regs.x10 offset mismatch");
_Static_assert(offsetof(struct cpu_regs, x11) == CPU_REGS_OFFSET_X11, "cpu_regs.x11 offset mismatch");
_Static_assert(offsetof(struct cpu_regs, x12) == CPU_REGS_OFFSET_X12, "cpu_regs.x12 offset mismatch");
_Static_assert(offsetof(struct cpu_regs, x13) == CPU_REGS_OFFSET_X13, "cpu_regs.x13 offset mismatch");
_Static_assert(offsetof(struct cpu_regs, x14) == CPU_REGS_OFFSET_X14, "cpu_regs.x14 offset mismatch");
_Static_assert(offsetof(struct cpu_regs, x15) == CPU_REGS_OFFSET_X15, "cpu_regs.x15 offset mismatch");
_Static_assert(offsetof(struct cpu_regs, x16) == CPU_REGS_OFFSET_X16, "cpu_regs.x16 offset mismatch");
_Static_assert(offsetof(struct cpu_regs, x17) == CPU_REGS_OFFSET_X17, "cpu_regs.x17 offset mismatch");
_Static_assert(offsetof(struct cpu_regs, x18) == CPU_REGS_OFFSET_X18, "cpu_regs.x18 offset mismatch");
_Static_assert(offsetof(struct cpu_regs, x19) == CPU_REGS_OFFSET_X19, "cpu_regs.x19 offset mismatch");
_Static_assert(offsetof(struct cpu_regs, x20) == CPU_REGS_OFFSET_X20, "cpu_regs.x20 offset mismatch");
_Static_assert(offsetof(struct cpu_regs, x21) == CPU_REGS_OFFSET_X21, "cpu_regs.x21 offset mismatch");
_Static_assert(offsetof(struct cpu_regs, x22) == CPU_REGS_OFFSET_X22, "cpu_regs.x22 offset mismatch");
_Static_assert(offsetof(struct cpu_regs, x23) == CPU_REGS_OFFSET_X23, "cpu_regs.x23 offset mismatch");
_Static_assert(offsetof(struct cpu_regs, x24) == CPU_REGS_OFFSET_X24, "cpu_regs.x24 offset mismatch");
_Static_assert(offsetof(struct cpu_regs, x25) == CPU_REGS_OFFSET_X25, "cpu_regs.x25 offset mismatch");
_Static_assert(offsetof(struct cpu_regs, x26) == CPU_REGS_OFFSET_X26, "cpu_regs.x26 offset mismatch");
_Static_assert(offsetof(struct cpu_regs, x27) == CPU_REGS_OFFSET_X27, "cpu_regs.x27 offset mismatch");
_Static_assert(offsetof(struct cpu_regs, x28) == CPU_REGS_OFFSET_X28, "cpu_regs.x28 offset mismatch");
_Static_assert(offsetof(struct cpu_regs, x29) == CPU_REGS_OFFSET_X29, "cpu_regs.x29 offset mismatch");
_Static_assert(offsetof(struct cpu_regs, lr) == CPU_REGS_OFFSET_LR, "cpu_regs.lr offset mismatch");
_Static_assert(offsetof(struct cpu_regs, sp) == CPU_REGS_OFFSET_SP, "cpu_regs.sp offset mismatch");
_Static_assert(offsetof(struct cpu_regs, elr) == CPU_REGS_OFFSET_ELR, "cpu_regs.elr offset mismatch");
_Static_assert(offsetof(struct cpu_regs, spsr) == CPU_REGS_OFFSET_SPSR, "cpu_regs.spsr offset mismatch");
_Static_assert(offsetof(struct cpu_regs, esr) == CPU_REGS_OFFSET_ESR, "cpu_regs.esr offset mismatch");
_Static_assert(offsetof(struct cpu_regs, far) == CPU_REGS_OFFSET_FAR, "cpu_regs.far offset mismatch");
_Static_assert(offsetof(struct cpu_regs, hpfar) == CPU_REGS_OFFSET_HPFAR, "cpu_regs.hpfar offset mismatch");
_Static_assert(offsetof(struct cpu_regs, host_tpidr) == CPU_REGS_OFFSET_HOST_TPIDR,
	"cpu_regs.host_tpidr offset mismatch");
_Static_assert(offsetof(struct cpu_regs, exc_sp) == CPU_REGS_OFFSET_EXC_SP,
	"cpu_regs.exc_sp offset mismatch");
_Static_assert(offsetof(struct cpu_regs, reserved) == CPU_REGS_OFFSET_RESERVED,
	"cpu_regs.reserved offset mismatch");
_Static_assert(sizeof(struct cpu_regs) == CPU_REGS_OFFSET_LAST, "cpu_regs size mismatch");

#define STACK_FRAME_OFFSET_X19		0x0
#define STACK_FRAME_OFFSET_X20		0x8
#define STACK_FRAME_OFFSET_X21		0x10
#define STACK_FRAME_OFFSET_X22		0x18
#define STACK_FRAME_OFFSET_X23		0x20
#define STACK_FRAME_OFFSET_X24		0x28
#define STACK_FRAME_OFFSET_X25		0x30
#define STACK_FRAME_OFFSET_X26		0x38
#define STACK_FRAME_OFFSET_X27		0x40
#define STACK_FRAME_OFFSET_X28		0x48
#define STACK_FRAME_OFFSET_X29		0x50
#define STACK_FRAME_OFFSET_LR		0x58
#define STACK_FRAME_OFFSET_X0		0x60
#define STACK_FRAME_OFFSET_SPSR		0x68
#define STACK_FRAME_SIZE		0x80

struct stack_frame {
	uint64_t x19;
	uint64_t x20;
	uint64_t x21;
	uint64_t x22;
	uint64_t x23;
	uint64_t x24;
	uint64_t x25;
	uint64_t x26;
	uint64_t x27;
	uint64_t x28;
	uint64_t x29;
	uint64_t lr;
	uint64_t x0;
	uint64_t spsr;
	uint64_t magic;
	uint64_t reserved;
};

#define cpu_relax()	arch_asm_pause()
#define NR_CPUS		MAX_PCPU_NUM

#define LONG_BYTEORDER	3
#define BYTES_PER_LONG	(1 << LONG_BYTEORDER)
#define BITS_PER_LONG	(BYTES_PER_LONG << 3)

#define CPU_STACK_ALIGN	16UL

static inline uint64_t read_daif(void)
{
	uint64_t val;

	asm volatile ("mrs %0, daif" : "=r" (val));
	return val;
}

static inline uint16_t arch_get_pcpu_id(void)
{
	uint64_t pcpu_id;

	asm volatile ("mrs %0, tpidr_el2" : "=r" (pcpu_id));
	return (uint16_t)pcpu_id;
}

static inline void arch_set_current_pcpu_id(uint16_t pcpu_id)
{
	asm volatile ("msr tpidr_el2, %0" : : "r" ((uint64_t)pcpu_id));
}

static inline void arch_asm_pause(void)
{
	asm volatile ("yield" ::: "memory");
}

static inline void arch_local_irq_enable(void)
{
	asm volatile ("msr daifclr, #2" ::: "memory");
}

static inline void arch_local_irq_disable(void)
{
	asm volatile ("msr daifset, #2" ::: "memory");
}

static inline void arch_local_irq_save(uint64_t *flags_ptr)
{
	uint64_t flags = read_daif();

	arch_local_irq_disable();
	*flags_ptr = flags;
}

static inline void arch_local_irq_restore(uint64_t flags)
{
	if ((flags & DAIF_IRQ) == 0UL) {
		arch_local_irq_enable();
	} else {
		arch_local_irq_disable();
	}
}

static inline void arch_pre_user_access(void)
{
}

static inline void arch_post_user_access(void)
{
}

static inline bool need_offline(uint16_t pcpu_id)
{
	(void)pcpu_id;
	return false;
}

static inline void stac(void) {}
static inline void clac(void) {}

void wait_sync_change(volatile const uint64_t *sync, uint64_t wake_sync);
void init_percpu_mpidr(uint64_t bsp_mpidr);
uint16_t get_pcpu_id_from_mpidr(uint64_t mpidr);

#endif /* ASSEMBLER */

#endif /* ARM64_CPU_H */
