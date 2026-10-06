//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -fno-builtin %s -o %t
// RUN: %ptsan_musl_strict -fno-builtin -DZERO_ID %s -o %t.zero
// RUN: %ptsan_musl_strict -fno-builtin -DDEFAULT_ID %s -o %t.default
// RUN: args=''; for i in 0 1 2 3 4 5 6 7 8 9 10 11 12; do \
// RUN:   %run %t $args && \
// RUN:   not %run %t.zero $args 2>&1 | FileCheck %s && \
// RUN:   not %run %t.default $args 2>&1 | FileCheck %s || exit 1; \
// RUN:   args="$args x"; \
// RUN: done
// CHECK: Found bad {{.*}} access

// Check reserved-ID rejection through both musl's instrumented string routines
// and the raw-primitive wrappers. Zero-length operations must still succeed.
#include "../string_scanning.c"
