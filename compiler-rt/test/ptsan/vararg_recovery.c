//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error=0 \
// RUN:   -DTEST_CASE=EMPTY_OVERFLOW %S/vararg_strict.c -o %t
// RUN: %t > %t.out 2>&1
// RUN: FileCheck %s < %t.out

// Continuing after a vararg overread must still release the va_list backing IDs.
// ID32 does not support recovery after a failed check.
// CHECK: Found bad POINT access
// CHECK: Unreclaimed IDs at exit: 0
