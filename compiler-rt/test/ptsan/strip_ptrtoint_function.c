//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// A selected function can reconstruct NULL from pointer integers, as in GCC's
// obstack release macro, without losing IDs in neighboring round-trip functions.
// Subtraction in a second selected function must not decode raw integers twice.
// Separate translation units keep these functions present when PTSan runs.
// RUN: split-file %s %t.dir
// RUN: %clang_ptsan_id32 -O3 -mllvm -ptsan-strip-ptrtoint-function=reconstruct -mllvm -ptsan-strip-ptrtoint-function=difference -mllvm -ptsan-clear-ids-on-arith=1 %t.dir/helper.c %t.dir/main.c -o %t
// RUN: %run %t

//--- helper.c
#include <stdint.h>
#include <stddef.h>

char *reconstruct(char *p, ptrdiff_t *saved) {
  *saved = (char *)0 - p;
  return p + *saved;
}

uintptr_t difference(char *p, char *q) {
  return (uintptr_t)p - (uintptr_t)q;
}

char *roundtrip(char *p, volatile uintptr_t *saved) {
  *saved = (uintptr_t)p;
  return (char *)*saved;
}

//--- main.c
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

char *reconstruct(char *, ptrdiff_t *);
uintptr_t difference(char *, char *);
char *roundtrip(char *, volatile uintptr_t *);

int main(void) {
  char *p = malloc(64);
  ptrdiff_t saved;
  volatile uintptr_t bits;
  assert(reconstruct(p, &saved) == NULL);
  assert(difference(p + 5, p) == 5);
  assert(difference(p, p + 5) == (uintptr_t)-5);
  assert(roundtrip(p, &bits) == p);
  // Compare integer bits too: pointer comparison may strip both operands.
  assert(bits == (uintptr_t)p);
  free(p);
  return 0;
}
