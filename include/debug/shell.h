/*
 * Copyright (C) 2018-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef SHELL_H
#define SHELL_H

#include <types.h>

void shell_init(void);
void shell_start(void);
void shell_kick(void);
bool shell_is_open(void);
bool shell_async_puts(const char *string_ptr);

#endif /* SHELL_H */
