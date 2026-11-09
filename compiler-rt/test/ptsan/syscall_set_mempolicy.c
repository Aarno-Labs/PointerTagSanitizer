//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error %s -o %t
// RUN: %run %t
// RUN: not %run %t bad 2>&1 | FileCheck %s
// RUN: %clang_ptsan_plain -O2 -mllvm -ptsan-abort-on-error %s -o %t.plain
// RUN: %run %t.plain
// RUN: not %run %t.plain bad 2>&1 | FileCheck %s

// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-abort-on-error %s -o %t
// RUN: %run %t
// RUN: not %run %t bad 2>&1 | FileCheck %s
// RUN: %clang_ptsan_plain -fsanitize-ptsan-32-bit-ids -DPTSAN_TEST_32_BIT_IDS -O2 -mllvm -ptsan-abort-on-error %s -o %t.plain
// RUN: %run %t.plain
// RUN: not %run %t.plain bad 2>&1 | FileCheck %s
// CHECK: Found bad libc access
// CHECK: Alloc size  0x7
// CHECK: Access size 0x8

#include "Inputs/syscall_set_mempolicy.c"
