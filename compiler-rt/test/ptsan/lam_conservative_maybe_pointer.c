// REQUIRES: ptsan-lam-u48-runtime
//
// Mirrors conservative GC scans such as SIOD's: raw machine words are cast to
// pointer type and rejected by range predicates before the only source-level
// dereference.  The LAM path currently loads bounds for the raw candidate word
// before proving it reaches that dereference.
//
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -mllvm -ptsan-quiet-errors -o %exe.ptsan %s
// RUN: ./%exe.ptsan | FileCheck %s
// RUN: %clang_ptsan -g -O2 -mllvm -ptsan-quiet-errors -mllvm -ptsan-lam -o %exe.lam %s
// RUN: ./%exe.lam | FileCheck %s

// CHECK: hits=0

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct obj {
  int type;
  int mark;
  long payload;
};

typedef struct obj *LISP;

LISP *heaps;
long nheaps = 10;
long heap_size = 4;

__attribute__((noinline)) static long looks_pointerp(LISP p) {
  long j;
  LISP h;

  for (j = 0; j < nheaps; ++j) {
    if ((h = heaps[j]) && (p >= h) && (p < (h + heap_size)) &&
        (((char *)p - (char *)h) % sizeof(struct obj) == 0) &&
        p->type != 12) {
      return 1;
    }
  }

  return 0;
}

__attribute__((noinline)) static long scan_words(uintptr_t *words, long n) {
  long hits = 0;

  for (long i = 0; i < n; ++i) {
    LISP p = (LISP)words[i];
    if (looks_pointerp(p)) {
      ++hits;
    }
  }

  return hits;
}

int main(void) {
  heaps = (LISP *)calloc((size_t)nheaps, sizeof(LISP));
  if (!heaps) {
    return 1;
  }

  heaps[0] = (LISP)calloc((size_t)heap_size, sizeof(struct obj));
  if (!heaps[0]) {
    return 1;
  }

  uintptr_t words[] = {
      0xaaaaaaaaaaaaaaabULL,
  };

  printf("hits=%ld\n", scan_words(words, 1));
  return 0;
}
