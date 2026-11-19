//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: split-file %s %t.dir
// RUN: %clang_ptsan -O2 %t.dir/main.c %t.dir/difference.c -o %t
// RUN: %run %t

// An integer argument can contain an address even when its ptrtoint is not
// visible in the callee. Do not mistake it for a numeric offset and retag the
// result of subtracting two addresses from the same allocation.

//--- difference.c
#include <stdint.h>

uintptr_t difference(char *p, uintptr_t base) {
  return (uintptr_t)p - base;
}

//--- main.c
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

uintptr_t difference(char *p, uintptr_t base);

int main(void) {
  char *p = malloc(64);
  assert(p);
  assert(difference(p + 32, (uintptr_t)p) == 32);
  free(p);
}
