/*
 * Copyright (C) 2026 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <bare.h>

extern const uint8_t qemu_lk_image_start[];
extern const uint8_t qemu_lk_image_size[];
extern const uint8_t qemu_zephyr_image_start[];
extern const uint8_t qemu_zephyr_image_size[];

struct bare_boot_option bare_boot_options[] = {
	{
		.addr = (uint64_t)qemu_zephyr_image_start,
		.size = (uint64_t)qemu_zephyr_image_size,
		.tag = "zephyr",
	},
	{
		.addr = (uint64_t)qemu_lk_image_start,
		.size = (uint64_t)qemu_lk_image_size,
		.tag = "lk",
	},
};
uint16_t n_bare_boot_options = ARRAY_SIZE(bare_boot_options);
