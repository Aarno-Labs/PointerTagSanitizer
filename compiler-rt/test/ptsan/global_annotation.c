//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error %s -o %t
// RUN: %t

// Annotation strings live in llvm.metadata, which is not emitted as runtime
// storage. Registering bounds for them creates undefined assembler symbols.
// Ordinary annotated objects still need their normal instrumentation.
#include <assert.h>

__attribute__((annotate("object-metadata"))) int value = 41;

__attribute__((noinline, annotate("function-metadata"))) int increment(int *p) {
  return ++*p;
}

int main(void) {
  assert(increment(&value) == 42);
  assert(value == 42);
  return 0;
}
