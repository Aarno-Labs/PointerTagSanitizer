//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict %s -o %t
// RUN: %run %t
// RUN: %ptsan_musl_strict_regular %s -o %t.regular
// RUN: %run %t.regular
// RUN: %ptsan_musl_id32_strict %s -o %t.id32
// RUN: %run %t.id32
#include "../setjmp_cleanup.c"
