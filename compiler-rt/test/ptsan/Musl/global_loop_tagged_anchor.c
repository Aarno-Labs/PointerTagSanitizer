//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -O3 -fno-vectorize -fno-slp-vectorize -fno-unroll-loops -mllvm -ptsan-reoptimize -mllvm -ptsan-clear-ids-on-arith=0 -mllvm -ptsan-clear-ids-on-cmp=0 %s -o %t
// RUN: %run %t 64 > %t.good 2>&1
// RUN: FileCheck %s --check-prefix=GOOD < %t.good
// RUN: not %run %t 65 > %t.bad 2>&1
// RUN: FileCheck %s --check-prefix=BAD < %t.bad

// GOOD-NOT: Found bad
// GOOD: sum: 43680
// GOOD-NOT: Found bad
// BAD: Found bad LOOP access

#include "../loops/global_loop_tagged_anchor.c"
