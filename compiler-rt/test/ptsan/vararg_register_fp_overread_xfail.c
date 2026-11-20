//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// XFAIL: *
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error -o %t %s
// RUN: not %t > %t.out 2>&1
// RUN: FileCheck %s < %t.out

// Supply one double but read two, both from FP register-save slots.
// The backing ID covers all FP slots, not just the supplied arguments.
// Currently no complaint is emitted.
// CHECK: Found bad {{.*}} access
#define TEST_CASE REGISTER_FP_OVERREAD
#include "vararg_strict.c"
