// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 60 80 2> out.txt
// RUN: cat out.txt | FileCheck %s

// CHECK: {{Total checks called: [1-5]$}}

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

__attribute__((noinline))
void mc_row(uint8_t *dst, const uint8_t *src, unsigned sz) {
  for (unsigned x = 0; x < sz; x++) {
    int val = src[x];
    dst[x] = (uint8_t)(val >> 6);
  }
}

__attribute__((noinline))
int main(void) {
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
  for (size_t i = 0; i < dst_size; i++) {
    dst[i] = 0;
  }

  mc_row(dst, src, 64);

  // Tiny use so compiler can’t trivially delete everything
  printf("dst[0] = %u\n", (unsigned)dst[0]);

  free(dst);
  free(src);
  return 0;
}
