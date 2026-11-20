//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -fno-builtin -o %t %s
// RUN: not %run %t 2>&1 | FileCheck %s
// RUN: %ptsan_musl_strict -fno-builtin -DSTACK_IDS \
// RUN:   -mllvm -ptsan-no-stack-id-fast-path \
// RUN:   -o %t.stack %s
// RUN: not %run %t.stack 2>&1 | FileCheck %s
// RUN: not %run %t.stack x 2>&1 | FileCheck %s
// RUN: not %run %t.stack x x 2>&1 | FileCheck %s
// RUN: not %run %t.stack x x x 2>&1 | FileCheck %s
// RUN: not %run %t.stack x x x x 2>&1 | FileCheck %s
// CHECK-NOT: default ID produced
// CHECK: PTSan: ID space exhausted in strict mode
// CHECK-NOT: default ID produced

// Strict exhaustion must fail at allocation, before a fallback ID is returned.
// Exercise heap allocation and each stack-ID allocation helper.
#include "../id_exhaustion.c"
