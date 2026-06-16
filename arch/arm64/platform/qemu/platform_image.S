/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

	.section .rodata.lk_image, "a", %progbits
	.align 12
	.globl qemu_lk_image_start
qemu_lk_image_start:
	.incbin "sdk/images/lk.bin"
	.globl qemu_lk_image_end
qemu_lk_image_end:
	.globl qemu_lk_image_size
	.set qemu_lk_image_size, qemu_lk_image_end - qemu_lk_image_start
	.align 12

	.section .rodata.sima_linux_dtb, "a", %progbits
	.align 12
	.globl qemu_sima_linux_dtb_start
qemu_sima_linux_dtb_start:
	.incbin "sdk/images/linux/sima-linux.dtb"
	.globl qemu_sima_linux_dtb_end
qemu_sima_linux_dtb_end:
	.globl qemu_sima_linux_dtb_size
	.set qemu_sima_linux_dtb_size, qemu_sima_linux_dtb_end - qemu_sima_linux_dtb_start
	.align 12

	.section .rodata.zephyr_image, "a", %progbits
	.align 12
	.globl qemu_zephyr_image_start
qemu_zephyr_image_start:
	.incbin "sdk/images/zephyr.bin"
	.globl qemu_zephyr_image_end
qemu_zephyr_image_end:
	.globl qemu_zephyr_image_size
	.set qemu_zephyr_image_size, qemu_zephyr_image_end - qemu_zephyr_image_start
	.align 12
