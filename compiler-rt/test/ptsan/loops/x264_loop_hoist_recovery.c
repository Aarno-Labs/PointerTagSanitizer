//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan -O3 -g -fno-discard-value-names -fno-unsafe-math-optimizations -fcommon \
// RUN:   -mllvm -ptsan-reoptimize -mllvm -ptsan-clear-ids-on-arith=0 \
// RUN:   -mllvm -ptsan-clear-ids-on-cmp=0 -mllvm -ptsan-stack-temporal-safety=0 \
// RUN:   -mllvm -ptsan-preanalyze-loops=0 -mllvm -ptsan-no-loop-check-hoist \
// RUN:   -mllvm -ptsan-abort-on-error=0 -o %t %S/x264_loop_hoist_check_count.c
// RUN: %t 5 1 2> %t.err
// RUN: FileCheck %s < %t.err
// RUN: %t 5 0 2> %t.noden.err
// RUN: FileCheck %s < %t.noden.err

// Without loop hoisting, continuing through the invalid fifth row must execute
// its per-iteration checks too. The ordinary test checks termination and the
// valid-path counts in both encodings; this checks the completed error path.
// CHECK: Found bad COMPOUND access
// CHECK: Checks called:
// CHECK: POINT: 2
// CHECK: LOOP: 0
// CHECK: PREANALYZED_LOOP: 0
// CHECK: COMPOUND: 14
// CHECK: Total checks called: 16
