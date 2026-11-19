//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Removing doubled separators exercises end - current - 1, which LLVM can
// express with a complemented pointer integer. Stripping must preserve the
// memmove length, with and without the bit-twiddle compatibility option.

// RUN: %clang_ptsan -O2 -o %t %s
// RUN: %run %t | FileCheck %s
// RUN: %clang_ptsan -O2 -mllvm -ptsan-strip-pointer-bit-twiddles=12 -o %t.compat %s
// RUN: %run %t.compat | FileCheck %s

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__attribute__((noinline)) static void normalize_separators(char *s,
                                                           size_t length) {
  char *end = s + length;
  for (; s < end; ++s) {
    if (s[0] == ':' && s[1] == ':') {
      s[0] = '/';
      memmove(s + 1, s + 2, end - s - 1);
      --end;
    }
  }
}

int main(void) {
  char *text = malloc(9);
  assert(text);
  memcpy(text, "foo::bar", 9);

  normalize_separators(text, 8);
  assert(strcmp(text, "foo/bar") == 0);

  free(text);
  puts("subtraction preserved");
  return 0;
}

// CHECK: subtraction preserved
