//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: split-file %s %t.dir
// RUN: %clang_ptsan -O2 -c %t.dir/helper.c -o %t.dir/default.o
// RUN: %clang_ptsan -O2 -I%S %t.dir/main.c %t.dir/default.o -o %t.default
// RUN: %run %t.default
// RUN: %clang_ptsan -O2 -mllvm -ptsan-strip-ptrtoint -c %t.dir/helper.c -o %t.dir/raw.o
// RUN: %clang_ptsan -O2 -DRAW_INTEGERS -I%S %t.dir/main.c %t.dir/raw.o -o %t.raw
// RUN: %run %t.raw
// RUN: %clang_ptsan -O2 -mllvm -ptsan-strip-ptrtoint -mllvm -ptsan-no-opts -c %t.dir/helper.c -o %t.dir/no-opts.o
// RUN: %clang_ptsan -O2 -DRAW_INTEGERS -I%S %t.dir/main.c %t.dir/no-opts.o -o %t.no-opts
// RUN: %run %t.no-opts

// Only helper.c opts in. Ordinary pointer stores must retain their IDs, while
// integer stores deliberately contain raw addresses. Exercise the reassociated
// realloc expression with both tagged and raw inputs from the same allocation.

//--- helper.c
#include <stddef.h>
#include <stdint.h>

volatile uintptr_t saved_bits;
char *volatile saved_pointer;

void publish(char *p) {
  saved_bits = (uintptr_t)p;
  saved_pointer = p;
}

char *restore(void) { return (char *)saved_bits; }
uint32_t narrow(char *p) { return (uint32_t)(uintptr_t)p; }
int remaining(char *end, char *p, size_t n) { return end - p - n; }

//--- main.c
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "Inputs/ptsan_test_interface.h"

extern volatile uintptr_t saved_bits;
extern char *volatile saved_pointer;
void publish(char *);
char *restore(void);
uint32_t narrow(char *);
int remaining(char *, char *, size_t);

int main(void) {
  char *p = malloc(512);
  assert(p && get_id(p));
  publish(p + 16);
  assert(get_id(saved_pointer) == get_id(p));
  assert(ptsan_get_address(saved_pointer) == ptsan_get_address(p) + 16);
#ifdef RAW_INTEGERS
  uintptr_t raw = ptsan_get_address(p);
  assert(saved_bits == raw + 16);
  assert(get_id(restore()) == 0);
  assert(ptsan_get_address(restore()) == raw + 16);
  assert(narrow(p + 16) == (uint32_t)(raw + 16));
  assert(remaining(p + 256, (char *)raw, 128) == 128);
  assert(remaining((char *)raw + 256, p, 128) == 128);
#else
  assert(saved_bits == (uintptr_t)(p + 16));
  assert(get_id(restore()) == get_id(p));
#endif
  assert(remaining(p + 256, p, 128) == 128);
  free(p);
}
