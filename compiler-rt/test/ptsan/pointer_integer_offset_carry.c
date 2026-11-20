//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Flex-style end - pointer - count arithmetic must produce a numeric length,
// even when reassociation creates an intermediate carry into the ID bits.

// RUN: split-file %s %t.dir
// RUN: %clang_ptsan -O2 %t.dir/remaining.c %t.dir/main.c -o %t
// RUN: %run %t
// RUN: %clang_ptsan -O2 -mllvm -ptsan-no-opts %t.dir/remaining.c %t.dir/main.c -o %t.no-opts
// RUN: %run %t.no-opts

//--- remaining.c
#include <stddef.h>
#include <stdint.h>

uint32_t remaining(char *end, char *p, size_t n) {
  return (uint32_t)(end - p - n);
}

//--- main.c
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

uint32_t remaining(char *, char *, size_t);

int main(void) {
  char *p = malloc(512);
  assert(p);
  // Flex's vectorized copy loop uses this modular integer arithmetic. The
  // intermediate addition can carry into an ID; it must run on raw addresses.
  assert(remaining(p + 256, p, UINT32_MAX - 1ULL) == 258);
  free(p);
}
