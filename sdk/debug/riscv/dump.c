/*
 * Copyright (C) 2018-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <irq.h>
#include <asm/page.h>

#define CALL_TRACE_HIERARCHY_MAX	0x10U
#define DUMP_STACK_SIZE			0x800U

void asm_assert(__unused int32_t line, __unused const char *file, __unused const char *txt) {}

static void show_host_call_trace(uint64_t stack_phy, uint64_t s0, uint16_t pcpu_id)
{
	uint32_t i = 0U;
	uint32_t cb_hierarchy = 0U;
	uint64_t *fp = (uint64_t *)s0;
	uint64_t *sp = (uint64_t *)stack_phy;
	uint64_t dump_size = min(roundup(stack_phy, PAGE_SIZE) - stack_phy, DUMP_STACK_SIZE);

	/* TODO: pritf the delta between the actual load address and the config load address here */
	//pr_acrnlog("\r\n delta = (actual_load_address - config_hv_ram_start) = 0x%llx\r\n", get_hv_image_delta());
	pr_acrnlog("host stack: cpu_id = %hu\r\n", pcpu_id);
	for (i = 0U; i < (dump_size >> 5U); i++) {
		pr_acrnlog("addr(0x%lx)	0x%016lx  0x%016lx  0x%016lx  0x%016lx\r\n",
			(stack_phy + (i * 32U)), sp[i * 4U],
			sp[(i * 4U) + 1U], sp[(i * 4U) + 2U],
			sp[(i * 4U) + 3U]);
	}
	pr_acrnlog("\r\n");

	pr_acrnlog("host call trace:\r\n");

	/* if enable compiler option(no-omit-frame-pointer)  the stack layout
	 * should be like this when call a function for risc-v
	 *
	 *                  |                    |
	 *       fp + 16    |  last fp pointer   |
	 *       fp + 8     |  return address    |    push ra
	 *                  |                    |    sw fp s0
	 *
	 *       rsp        |                    |
	 *
	 *
	 *  if the address is invalid, it will cause hv page fault
	 *  then halt system */

	while (cb_hierarchy < CALL_TRACE_HIERARCHY_MAX) {
		pr_acrnlog("----> 0x%016lx\r\n", *(uint64_t *)(fp - 1));

		if (*fp == SP_BOTTOM_MAGIC) {
			break;
		}

		fp = (uint64_t *)(*(fp - 2));
		cb_hierarchy++;
	}

	pr_acrnlog("\r\n");
}

/* General purpose registers. */
// s2; s3; s4; s5; s6; s7; s8; s9; s10; s11;
// t3; t4; t5; t6;

void dump_intr_excp_frame(const struct intr_excp_ctx *ctx)
{
	pr_acrnlog("================================================");
	pr_acrnlog("================================\n");

	/* Dump host register*/
	pr_acrnlog("host registers:\r\n");
	pr_acrnlog("=  cause=0x%016llx  epc=0x%016llx\n",
			ctx->regs.cause, ctx->regs.epc);
	pr_acrnlog("=  tval=0x%016llx  status=0x%016llx, scratch=0x%016llx\n",
			ctx->regs.tval, ctx->regs.status, ctx->regs.scratch);

	pr_acrnlog("=     ra=0x%016llx  sp=0x%016llx  gp=0x%016llx, tp=0x%016llx\n",
			ctx->regs.ra, ctx->regs.sp, ctx->regs.gp, ctx->regs.tp);
	/* Temporary registers */
	pr_acrnlog("=     t0=0x%016llx  t1=0x%016llx  t2=0x%016llx\n",
			ctx->regs.t0, ctx->regs.t1, ctx->regs.t2);
	/* Callee-saved registers */
	pr_acrnlog("=     s0=0x%016llx  s1=0x%016llx\n",
			ctx->regs.s0, ctx->regs.s1);

	/* Argument registers */
	pr_acrnlog("=     a0=0x%016llx  a1=0x%016llx, a2=0x%016llx  a3=0x%016llx\n",
			ctx->regs.a0, ctx->regs.a1, ctx->regs.a2, ctx->regs.a3);
	pr_acrnlog("=     a4=0x%016llx  a5=0x%016llx, a6=0x%016llx  a7=0x%016llx\n",
			ctx->regs.a4, ctx->regs.a5, ctx->regs.a6, ctx->regs.a7);

	/* Callee-saved registers */
	pr_acrnlog("=     s2=0x%016llx  s3=0x%016llx, s4=0x%016llx  s5=0x%016llx\n",
			ctx->regs.s2, ctx->regs.s3, ctx->regs.s4, ctx->regs.s5);
	pr_acrnlog("=     s6=0x%016llx  s7=0x%016llx, s8=0x%016llx  s9=0x%016llx\n",
			ctx->regs.s6, ctx->regs.s7, ctx->regs.s8, ctx->regs.s9);
	pr_acrnlog("=     s10=0x%016llx  s11=0x%016llx\n",
			ctx->regs.s10, ctx->regs.s11);
	/* Temporary registers */
	pr_acrnlog("=     t3=0x%016llx  t4=0x%016llx  t5=0x%016llx, t6=0x%016llx\n",
			ctx->regs.t3, ctx->regs.t4, ctx->regs.t5, ctx->regs.t6);
	pr_acrnlog("\r\n");

	pr_acrnlog("=====================================================");
	pr_acrnlog("===========================\n");
}

void dump_exception(const struct intr_excp_ctx *ctx, uint16_t pcpu_id)
{
	/* Dump host context */
	dump_intr_excp_frame(ctx);

	/* Show host stack */
	show_host_call_trace(ctx->regs.sp, ctx->regs.s0, pcpu_id);
}
