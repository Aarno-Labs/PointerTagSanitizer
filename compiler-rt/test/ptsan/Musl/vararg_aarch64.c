//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: aarch64-target-arch
// RUN: %ptsan_musl_strict -mllvm -ptsan-print-stats %s -o %t
// RUN: %run %t 2>&1 | FileCheck %s --check-prefix=CLEAN
// RUN: %ptsan_musl_strict -O0 %s -o %t.o0
// RUN: %run %t.o0
// RUN: %ptsan_musl_strict -mllvm -ptsan-no-opts %s -o %t.noopts
// RUN: %run %t.noopts
// RUN: %ptsan_musl_strict -mllvm -ptsan-stack-ids-per-bucket=1 %s -o %t.small
// RUN: %run %t.small
// CHECK: Found bad {{.*}} access
// CLEAN-NOT: Found bad
// CLEAN: Unreclaimed IDs at exit: 0
// CLEAN-NOT: Found bad

#include "../vararg_aarch64.c"
