// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_nonstrict -o %t %S/../frexp_libcall.c
// RUN: %run %t

// The backend-generated frexp call passes an untagged exponent temporary.
// Non-strict mode accepts it; the strict case has its own XFAIL test.
