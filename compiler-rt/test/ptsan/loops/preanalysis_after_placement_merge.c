// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_raw -fsanitize=ptsan -O2 -g -fno-discard-value-names \
// RUN:   -fno-vectorize -fno-slp-vectorize -fno-unroll-loops \
// RUN:   -mllvm -ptsan-runtime-validate -mllvm -ptsan-print-stats \
// RUN:   -o %exe %s
// RUN: ./%exe 4 2> good.err && FileCheck %s --check-prefixes=GOOD,STATS < good.err
// RUN: ./%exe 5 2> bad.err && FileCheck %s --check-prefixes=BAD,STATS < bad.err

// Regression coverage for delayed preanalysis elision: access checks are first
// placed normally, then removed from the explicit check plan when covered by a
// loop-preanalysis check. Check merging is intentionally left enabled.

// GOOD-NOT: Found bad
// BAD: Found bad PREANALYZED_LOOP access
// STATS: Checks called:
// STATS: LOOP: 0
// STATS: PREANALYZED_LOOP: 1
// STATS: COMPOUND: 0

#include <stdint.h>
#include <stdlib.h>

__attribute__((noinline))
void write_loop(uint8_t *buf, unsigned n) {
  volatile uint8_t *v = buf;
  for (unsigned i = 0; i < n; ++i) {
    v[i] = (uint8_t)i;
  }
}

int main(int argc, char **argv) {
  unsigned n = argc > 1 ? (unsigned)atoi(argv[1]) : 4;
  uint8_t *buf = (uint8_t *)malloc(4);
  if (!buf) {
    return 1;
  }

  write_loop(buf, n);
  free(buf);
  return 0;
}
