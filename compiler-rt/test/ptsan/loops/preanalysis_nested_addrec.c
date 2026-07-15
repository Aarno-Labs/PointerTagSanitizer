// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -O2 -fno-vectorize -fno-slp-vectorize -fno-unroll-loops -o %exe %s
// RUN: ./%exe 4 2> err && FileCheck %s --check-prefixes=GOOD,STATS < err
// RUN: ./%exe 5 2> err && FileCheck %s --check-prefixes=BAD,STATS < err

// GOOD-NOT: Found bad
// BAD: Found bad PREANALYZED_LOOP access
// STATS: Checks called:
// STATS: LOOP: 0
// STATS: PREANALYZED_LOOP: 1

#include <stdint.h>
#include <stdlib.h>

volatile unsigned sink;

__attribute__((noinline))
void nested(uint8_t *buf, unsigned rows) {
  for (unsigned i = 0; i < rows; ++i) {
    for (unsigned j = 0; j < 8; ++j) {
      unsigned idx = i * 8 + j;
      buf[idx] = (uint8_t)idx;
    }
  }
}

int main(int argc, char **argv) {
  unsigned rows = argc > 1 ? (unsigned)atoi(argv[1]) : 4;
  uint8_t *buf = malloc(32);

  nested(buf, rows);
  sink += buf[0];

  free(buf);
  return 0;
}
