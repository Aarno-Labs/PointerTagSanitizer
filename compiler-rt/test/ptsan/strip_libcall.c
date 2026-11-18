// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: split-file %s %t
// RUN: %clang_raw -O2 -mllvm -ptsan-strip-ids -c %t/prefix.c -o %t/prefix.o
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error %t/main.c %t/prefix.o -o %t/test
// RUN: %t/test
// RUN: %clang_raw -O2 -mllvm -ptsan-strip-ids -fsanitize-ptsan-32-bit-ids -c %t/prefix.c -o %t/prefix.id32.o
// RUN: %clang_ptsan_id32 -O2 %t/main.c %t/prefix.id32.o -o %t/test.id32
// RUN: %t/test.id32

// A constant-length comparison may become ordinary loads before stripping.
// Strip-only code must accept tagged arguments in either form; it does not
// promise bounds/lifetime checking, including incidental interceptor checks.
// Calls that survive stripping still must not expand into unstripped loads
// later during codegen.
//--- prefix.c
int has_prefix(const char *p) {
  return __builtin_memcmp(p, "_Z", 2) == 0;
}

//--- main.c
#include <assert.h>
#include <stdlib.h>

extern int has_prefix(const char *);

int main(void) {
  char *p = malloc(3);
  assert(p);
  p[0] = '_';
  p[1] = 'Z';
  p[2] = '\0';
  assert(has_prefix(p));
  assert(!has_prefix(p + 1));
  free(p);
  return 0;
}
