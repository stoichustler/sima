/*
 * Copyright (C) 2026 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_GUEST_VPL011_H
#define ARM64_GUEST_VPL011_H

#include <types.h>

struct acrn_vm;
struct io_request;

void arm64_vpl011_init_vm(struct acrn_vm *vm);
int32_t arm64_vpl011_mmio_handler(struct io_request *io_req, void *handler_private_data);

#endif /* ARM64_GUEST_VPL011_H */
