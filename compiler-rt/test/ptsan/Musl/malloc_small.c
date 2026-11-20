//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Heap ID32 bounds must accept the last byte and reject both adjacent bytes.
// Reuse the C reproducer exercised by the host-libc ID32 suite.
// RUN: %ptsan_musl_id32_nonstrict %S/../malloc_small.c -o %t.nonstrict
// RUN: %run %t.nonstrict 19
// RUN: not %run %t.nonstrict 20 2>&1 | FileCheck %s
// RUN: not %run %t.nonstrict -1 2>&1 | FileCheck %s
// RUN: %ptsan_musl_id32_strict %S/../malloc_small.c -o %t.strict
// RUN: %run %t.strict 19
// RUN: not %run %t.strict 20 2>&1 | FileCheck %s
// RUN: not %run %t.strict -1 2>&1 | FileCheck %s
// CHECK: Found bad POINT access
