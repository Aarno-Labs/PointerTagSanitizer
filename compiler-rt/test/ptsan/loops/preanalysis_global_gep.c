// RUN: rm -rf %t && mkdir -p %t && cd %t

// First build: ptsan with -mllvm -ptsan-no-opts
// RUN: %clang_ptsan -g -O2 -mllvm -ptsan-no-opts -o %exe.noopts %s
// RUN: ./%exe.noopts 16  2> err && FileCheck %s --check-prefixes=NOOPT,GOOD < err
// RUN: ./%exe.noopts 64  2> err && FileCheck %s --check-prefixes=NOOPT,GOOD < err
// RUN: ./%exe.noopts 65  2> err && FileCheck %s --check-prefixes=NOOPT,BAD  < err
// RUN: ./%exe.noopts 100 2> err && FileCheck %s --check-prefixes=NOOPT,BAD  < err

// Second build: normal ptsan-optimized build
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 16  2> err && FileCheck %s --check-prefixes=OPT,GOOD < err
// RUN: ./%exe 64  2> err && FileCheck %s --check-prefixes=OPT,GOOD < err
// RUN: ./%exe 65  2> err && FileCheck %s --check-prefixes=OPT,BAD  < err
// we crash on this one, so we don't get the end stats
// RUN: not --crash ./%exe 100 2> err && FileCheck %s --check-prefixes=BAD  < err

// ==================== FileCheck patterns ====================

// BAD runs (off-by-one, off-by-much) must report an error.
// BAD: Found bad

// GOOD runs (16, 64) must *not* report an error.
// GOOD-NOT: Found bad

// Optimized (vectorized / preanalyzed-loop) run expectations.
// We expect no raw LOOP checks, and a small number of PREANALYZED_LOOP checks.
// OPT: Checks called:
// OPT: LOOP: 0
// OPT: {{PREANALYZED_LOOP: [1-5]$}}
// OPT: {{Total checks called: [1-9]$}}

// No-opts runs: everything should be POINT checks, and Total == POINT.
// NOOPT: Checks called:
// NOOPT: POINT: [[C:[1-9][0-9]*]]
// NOOPT: Total checks called: [[C]]

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

unsigned szs[10];

__attribute__((noinline))
void mc_row(uint8_t *dst, const uint8_t *src) {
  unsigned sz = szs[8];
  for (unsigned x = 0; x < sz; x++) {
    int val = src[x];
    dst[x] = (uint8_t)(val >> 6);
  }
}

__attribute__((noinline))
int main(int argc, char **argv) {
  const int width = 64;
  const int src_stride = 80;

  size_t dst_size = (size_t)width;
  size_t src_size = (size_t)src_stride * 2;  // enough for s0/s1

  uint8_t *dst = malloc(dst_size);
  uint8_t *src = malloc(src_size);

  if (!dst || !src) {
    fprintf(stderr, "alloc failed\n");
    return 1;
  }

  for (size_t i = 0; i < src_size; i++) {
    src[i] = (uint8_t)(i & 0xFF);
  }

  for (int i = 0; i < 10; i++) {
    szs[i] = i * i;
  }

  if (argc < 2) {
    fprintf(stderr, "Usage: %s <index>\n", argv[0]);
    return 1;
  }

  szs[8] = atoi(argv[1]);

  mc_row(dst, src);

  // Tiny use so compiler can’t trivially delete everything
  printf("dst[0] = %u\n", (unsigned)dst[0]);

  free(dst);
  free(src);
  return 0;
}
