// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -O2 -fno-vectorize -fno-slp-vectorize -fno-unroll-loops -o %exe %s
// RUN: ./%exe 4 2> err && FileCheck %s < err

// CHECK-NOT: Found bad
// CHECK: Checks called:
// CHECK: LOOP: 0
// CHECK: PREANALYZED_LOOP: 1

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

volatile int sink;

__attribute__((noinline))
void side_a(uint8_t *p) {
  sink += p != 0;
}

__attribute__((noinline))
void side_b(uint8_t *p) {
  sink += p == 0;
}

__attribute__((noinline))
void touch(uint8_t *a, uint8_t *b, unsigned n, int which) {
  uint8_t *p;
  if (which) {
    side_a(a);
    p = a;
  } else {
    side_b(b);
    p = b;
  }

  for (unsigned i = 0; i < n; ++i)
    p[i] = (uint8_t)i;
}

int main(int argc, char **argv) {
  uint8_t *a = malloc(64);
  uint8_t *b = malloc(64);
  unsigned n = argc > 1 ? (unsigned)atoi(argv[1]) : 4;

  touch(a, b, n, argc & 1);
  fprintf(stderr, "%u\n", a[0] + b[0]);

  free(a);
  free(b);
  return 0;
}
