//===-- ptsan_musl_internal.h - Access to musl private state --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t *ptsan_musl_get_auxv(void);
void ptsan_musl_set_auxv(size_t *auxv);

void *ptsan_musl_self(void);
size_t ptsan_musl_thread_size(void);
void ptsan_musl_init_main_thread(void *tagged);
void ptsan_musl_init_child_thread(void *raw, void *tagged);
void ptsan_musl_thread_stack(void **top, size_t *size);
int ptsan_musl_is_c11_thread(const void *attr);

#ifdef __cplusplus
}
#endif
