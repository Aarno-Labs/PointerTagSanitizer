//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %t 2>&1 | FileCheck %s
// RUN: %ptsan_musl_strict \
// RUN:   -mllvm -ptsan-stack-ids-per-bucket=1 -o %t.smallbucket %s
// RUN: %t.smallbucket 2>&1 | FileCheck %s

// RUN: %ptsan_musl_strict -mllvm -ptsan-no-opts -o %t.noopts %s
// RUN: %t.noopts 2>&1 | FileCheck %s
// RUN: %ptsan_musl_strict -mllvm -ptsan-no-stack-id-fast-path -o %t.slow %s
// RUN: %t.slow 2>&1 | FileCheck %s
// RUN: %ptsan_musl_strict -mllvm -dom-tree-reachability-max-bbs-to-explore=1 -o %t.limit %s
// RUN: %t.limit 2>&1 | FileCheck %s

// CHECK-NOT: Found bad
// CHECK: dynamic-alloca-source-ok
// CHECK-NOT: Found bad

#include "Inputs/dynamic_alloca_source.c"
