//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan -O3 -c %s -o %t.o

// A hoisted bounds check may land between a positional vararg anchor and its
// call. Lowering must keep the anchor associated with the call.

int printf(const char *, ...);

void report(const int *values) {
  printf("%d\n", 0);
  printf("%d\n", values[1]);
}
