// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan -O3 -mllvm -ptsan-clear-ids-on-arith=0 -mllvm -ptsan-clear-ids-on-cmp=0 %s -o %t
// RUN: %run %t
// RUN: %clang_ptsan_id32 -O3 -mllvm -ptsan-clear-ids-on-arith=0 -mllvm -ptsan-clear-ids-on-cmp=0 %s -o %t.id32
// RUN: %run %t.id32
//
// Reduced from 500.perlbench_r. SCEV materializes a ptrtoaddr while reusing a
// source ptrtoint in the same range expression. Both casts must observe the
// same PTSan representation.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint32_t classes[256];
static volatile long saved_prefix;

#define digit(c) (classes[(unsigned char)(c)] & 2)

static inline int regcurly(const char *s) {
  if (*s++ != '{') {
    return 0;
  }
  if (!digit(*s)) {
    return 0;
  }
  while (digit(*s)) {
    ++s;
  }
  if (*s == ',') {
    ++s;
    while (digit(*s)) {
      ++s;
    }
  }
  return *s == '}';
}

__attribute__((noinline)) static long scan(const char *base, const char *s) {
  long prefix = s - base;
  saved_prefix = prefix;
  return prefix + regcurly(s);
}

int main(void) {
  for (unsigned i = '0'; i <= '9'; ++i) {
    classes[i] = 2;
  }

  char *s = strdup("\\N{3,4}");
  if (!s) {
    return 1;
  }
  long result = scan(s, s + 2);
  free(s);
  return result != 3;
}
