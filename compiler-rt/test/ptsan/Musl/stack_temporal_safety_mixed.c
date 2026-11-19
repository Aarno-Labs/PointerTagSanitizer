//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Check mixed stack-temporal translation units against instrumented strict musl.
// RUN: %ptsan_musl_strict -O2 -mllvm -ptsan-id-bucket-size=64 -mllvm -ptsan-stack-temporal-safety=1 -DFRAME_KIND=1 -c %S/../stack_temporal_safety_mixed.c -o %t.id16.strong.o
// RUN: %ptsan_musl_strict -O2 -mllvm -ptsan-id-bucket-size=64 -mllvm -ptsan-stack-temporal-safety=0 -DFRAME_KIND=0 -c %S/../stack_temporal_safety_mixed.c -o %t.id16.weak.o
// RUN: %ptsan_musl_strict -O2 -mllvm -ptsan-id-bucket-size=64 -mllvm -ptsan-stack-temporal-safety=1 %S/../stack_temporal_safety_mixed.c %t.id16.strong.o %t.id16.weak.o -o %t.id16
// RUN: %run %t.id16 2>&1 | FileCheck %S/../stack_temporal_safety_mixed.c --check-prefix=PASS
// RUN: not %run %t.id16 uar 2>&1 | FileCheck %S/../stack_temporal_safety_mixed.c --check-prefix=UAR
//
// RUN: %ptsan_musl_id32_strict -O2 -mllvm -ptsan-id-bucket-size=64 -mllvm -ptsan-stack-temporal-safety=1 -DFRAME_KIND=1 -c %S/../stack_temporal_safety_mixed.c -o %t.id32.strong.o
// RUN: %ptsan_musl_id32_strict -O2 -mllvm -ptsan-id-bucket-size=64 -mllvm -ptsan-stack-temporal-safety=0 -DFRAME_KIND=0 -c %S/../stack_temporal_safety_mixed.c -o %t.id32.weak.o
// RUN: %ptsan_musl_id32_strict -O2 -mllvm -ptsan-id-bucket-size=64 -mllvm -ptsan-stack-temporal-safety=1 %S/../stack_temporal_safety_mixed.c %t.id32.strong.o %t.id32.weak.o -o %t.id32
// RUN: %run %t.id32 2>&1 | FileCheck %S/../stack_temporal_safety_mixed.c --check-prefix=PASS
// RUN: not %run %t.id32 uar 2>&1 | FileCheck %S/../stack_temporal_safety_mixed.c --check-prefix=UAR
