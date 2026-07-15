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
// OPT: {{PREANALYZED_LOOP: [1-9]$}}
// OPT: {{Total checks called: [1-9]$}}

// No-opts runs: everything should be POINT checks, and Total == POINT.
// NOOPT: Checks called:
// NOOPT: POINT: [[C:[1-9][0-9]*]]
// NOOPT: Total checks called: [[C]]


#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

unsigned szs[10];

/*
 * Exercise: binops and GEP with a scaled index (x * 2).
 * Expression to serialize is src[idx] with idx = x * 2.
 */
__attribute__((noinline))
void mc_row_stride2(uint8_t *dst, const uint8_t *src) {
  unsigned sz = szs[7];  // 25 when szs[i] = i * i
  for (unsigned x = 0; x < sz; x++) {
    unsigned idx = x * 2;       // mul
    int val = src[idx];         // GEP with scaled index
    dst[x] = (uint8_t)(val >> 5);
  }
}


__attribute__((noinline))
int main(int argc, char **argv) {
  const int width = 64;
  const int src_stride = 80;

  size_t dst_size = (size_t)width;
  size_t src_size = (size_t)src_stride * 2;  // enough for all tests

  uint8_t *dst = (uint8_t *)malloc(dst_size);
  uint8_t *src = (uint8_t *)malloc(src_size);

  if (!dst || !src) {
    fprintf(stderr, "alloc failed\n");
    return 1;
  }

  for (size_t i = 0; i < src_size; i++) {
    src[i] = (uint8_t)(i & 0xFF);
  }

  // Initialize szs with some sane values; we then override szs[7].
  for (int i = 0; i < 10; i++) {
    szs[i] = (unsigned)(i * i);
  }
  /* szs[5] = 25, szs[7] = 49, szs[8] = 64 before override */

  if (argc < 2) {
    fprintf(stderr, "Usage: %s <index>\n", argv[0]);
    return 1;
  }

  int sz_override = atoi(argv[1]);

  // Override szs[7] with the CLI-controlled size so the loop bound
  // still comes from a global load in mc_row_stride2.
  szs[7] = sz_override;

  mc_row_stride2(dst, src);

  // Tiny uses so compiler can't trivially delete things.
  printf("dst[0]  = %u\n", (unsigned)dst[0]);
  printf("dst[10] = %u\n", (unsigned)dst[10]);
  printf("dst[20] = %u\n", (unsigned)dst[20]);
  printf("dst[30] = %u\n", (unsigned)dst[30]);

  free(dst);
  free(src);
  return 0;
}