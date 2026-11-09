//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Strict string operations must reject reserved IDs when reading bytes, but
// accept zero-length operations. Require the marker after the zero-length
// checks so an earlier complaint cannot satisfy the negative test.
// RUN: %ptsan_musl_strict -O2 -fno-builtin -mllvm -ptsan-abort-on-error -DSTRICT_RESERVED_ID -DZERO_ID %S/../string_scanning.c -o %t.zero
// RUN: %ptsan_musl_strict -O2 -fno-builtin -mllvm -ptsan-abort-on-error -DSTRICT_RESERVED_ID -DDEFAULT_ID %S/../string_scanning.c -o %t.default
// RUN: args=''; for i in 0 1 2 3 4 5 6 7 8 9 10 11 12; do \
// RUN:   not %run %t.zero $args 2>&1 | FileCheck %s && \
// RUN:   not %run %t.default $args 2>&1 | FileCheck %s || exit 1; \
// RUN:   args="$args x"; \
// RUN: done
// CHECK-NOT: Found bad
// CHECK: Checking nonzero string operation
// CHECK: Found bad {{(read|POINT)}} access
// CHECK: (Alloc size 0x0)
// CHECK: (Access size 0x1)
