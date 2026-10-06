//===-- pthread_impl.h - Complete PTSan musl syscall overlaying ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Activate PTSan's syscall overlay before musl defines inline futex helpers.
// Their syscall macros are expanded when the header is parsed.
//
//===----------------------------------------------------------------------===//

#include "syscall.h"
#include_next "pthread_impl.h"

// Keep the hardware thread pointer raw; instrumented C uses its tagged alias.
void *ptsan_musl_self(void);
#undef __pthread_self
#define __pthread_self() ((pthread_t)ptsan_musl_self())
