//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: not %run %t 2>&1 | FileCheck %s
// CHECK: PTSan: ID space exhausted in strict mode

// Repeated alloca must terminate on exhaustion rather than use a fallback ID.
#include "../dynamic_alloca_loop_exhaustion.c"
