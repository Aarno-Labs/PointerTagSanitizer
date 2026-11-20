//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: split-file %s %t.dir
// RUN: %clang_ptsan -O2 %t.dir/main.c %t.dir/choose.c -o %t
// RUN: %run %t 0
// RUN: %run %t 1

// Reduced from Lua's saved stack offsets. Optimization shares the base's
// ptrtoint through an integer PHI. That is still an address, not a numeric
// offset: the final subtraction must produce an ordinary ptrdiff_t.

//--- state.h
#include <stddef.h>
struct state {
  char *base;
  char *top;
};
char *choose(struct state *s);

//--- main.c
#include "state.h"
#include <assert.h>
#include <stdlib.h>

__attribute__((noinline))
ptrdiff_t saved(struct state *s, int flag, ptrdiff_t *first) {
  ptrdiff_t prior = 0;
  if (flag) {
    char *p = choose(s);
    prior = p - s->base;
  }
  *first = prior;
  return s->top - s->base;
}

int main(int argc, char **argv) {
  assert(argc == 2);
  int flag = argv[1][0] == '1';
  char *p = malloc(64);
  assert(p);
  struct state s = {p, p + 32};
  ptrdiff_t first;
  assert(saved(&s, flag, &first) == 32);
  assert(first == (flag ? 8 : 0));
  free(p);
}

//--- choose.c
#include "state.h"

char *choose(struct state *s) {
  return s->base + 8;
}
