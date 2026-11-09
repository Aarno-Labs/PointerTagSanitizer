//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: ptsan-instrumented-musl
// RUN: %ptsan_musl_id32_strict -O2 -fno-strict-overflow -DTEST_STRICT %s -o %t
// RUN: %run %t
// RUN: not %run %t bad 2>&1 | FileCheck %s
// CHECK: Found bad POINT access
// CHECK-SAME: Ptr ID: 0x0

// An unrepresentable payload becomes null in strict mode. Check both the
// returned value and rejection of a subsequent access through it.
#include "../id32_packing_overflow.c"
