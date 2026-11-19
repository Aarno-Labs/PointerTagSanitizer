//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -O2 -pthread -mllvm -ptsan-stack-temporal-safety=0 %S/../setjmp_cleanup.c -o %t
// RUN: %run %t 2>&1 | FileCheck %s
// RUN: %ptsan_musl_strict -O2 -pthread -DTEST_TEMPORAL=1 %S/../setjmp_cleanup.c -o %t.temporal
// RUN: %run %t.temporal 2>&1 | FileCheck %s
// RUN: %ptsan_musl_strict -O2 -pthread -DTEST_TEMPORAL=1 -mllvm -ptsan-no-stack-id-fast-path %S/../setjmp_cleanup.c -o %t.slow
// RUN: %run %t.slow 2>&1 | FileCheck %s
// RUN: %ptsan_musl_strict -O2 -pthread -mllvm -ptsan-stack-id-thread-buckets=0 -DTEST_TEMPORAL=1 %S/../setjmp_cleanup.c -o %t.nobuckets
// RUN: %run %t.nobuckets 2>&1 | FileCheck %s

// longjmp must retire skipped ID32 stack objects without losing live ones.
// RUN: %ptsan_musl_id32_strict -pthread -DTEST_TEMPORAL=1 %S/../setjmp_cleanup.c -o %t.id32
// RUN: %run %t.id32 2>&1 | FileCheck %s
// CHECK-NOT: Found bad
// CHECK: setjmp-cleanup-ok
// CHECK-NOT: Found bad
