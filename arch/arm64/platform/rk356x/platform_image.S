/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

	.section .rodata, "a"
	.align 12
	.globl rk356x_lk_image_start
rk356x_lk_image_start:
	.incbin "sdk/images/lk.bin"
	.globl rk356x_lk_image_end
rk356x_lk_image_end:
	.globl rk356x_lk_image_size
	.set rk356x_lk_image_size, rk356x_lk_image_end - rk356x_lk_image_start

	.align 12
	.globl rk356x_zephyr_image_start
rk356x_zephyr_image_start:
	.incbin "sdk/images/zephyr.bin"
	.globl rk356x_zephyr_image_end
rk356x_zephyr_image_end:
	.globl rk356x_zephyr_image_size
	.set rk356x_zephyr_image_size, rk356x_zephyr_image_end - rk356x_zephyr_image_start
