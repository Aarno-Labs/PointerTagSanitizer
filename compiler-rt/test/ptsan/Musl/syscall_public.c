//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict %s -o %t
// RUN: %run %t
// RUN: not %run %t bad 2>&1 | FileCheck %s

// Public syscall varargs must reach the shared ID32 decoding and bounds checks.
// RUN: %ptsan_musl_id32_nonstrict %s -o %t.id32
// RUN: %run %t.id32
// RUN: not %run %t.id32 bad 2>&1 | FileCheck %s
// RUN: %ptsan_musl_id32_strict %s -o %t.id32.strict
// RUN: %run %t.id32.strict
// RUN: not %run %t.id32.strict bad 2>&1 | FileCheck %s
// CHECK: Found bad libc access
// CHECK: Alloc size  0x4
// CHECK: Access size 0x5

#include "../Inputs/syscall_public.c"
