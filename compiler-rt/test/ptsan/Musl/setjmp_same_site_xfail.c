//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// XFAIL: *
// RUN: %ptsan_musl_strict_regular -O0 %s -o %t
// RUN: not %run %t single-checkpoint 2>&1 | FileCheck %S/../setjmp_same_site_xfail.c
// RUN: not %run %t 2>&1 | FileCheck %S/../setjmp_same_site_xfail.c
#include "../setjmp_same_site_xfail.c"
