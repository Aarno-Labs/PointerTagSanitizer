// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: aarch64-target-arch
// RUN: %ptsan_musl_nonstrict %S/../vararg_aarch64.c -o %t
// RUN: %run %t
// RUN: not %run %t gpr 2>&1 | FileCheck %S/../vararg_aarch64.c
// RUN: not %run %t fp 2>&1 | FileCheck %S/../vararg_aarch64.c
// RUN: not %run %t empty 2>&1 | FileCheck %S/../vararg_aarch64.c

// Variadic bounds checks must still reject overreads in non-strict mode.
