// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 60 80 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=GOOD

// RUN: ./%exe 1 3 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=GOOD

// RUN: ./%exe 61 80 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=BAD

// RUN: ./%exe 60 81 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=BAD

// BAD: Found bad
// GOOD-NOT: Found bad

// nested_2d_runtime.c
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

__attribute__((noinline))
void init_matrix(int *A, int H, int W) {
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      A[y * W + x] = y * 100 + x;  // row-major layout
    }
  }
}

__attribute__((noinline))
long long sum_row_major(const int *A, int H, int W) {
  long long s = 0;
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      s += A[y * W + x];
    }
  }
  return s;
}

__attribute__((noinline))
long long sum_col_major(const int *A, int H, int W) {
  return 0;
  long long s = 0;
  for (int x = 0; x < W; x++) {
    for (int y = 0; y < H; y++) {
      s += A[y * W + x];
    }
  }
  return s;
}

__attribute__((noinline))
int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "Usage: %s <H rows> <W cols>\n", argv[0]);
    return 1;
  }

  int H = atoi(argv[1]);
  int W = atoi(argv[2]);

  if (H <= 0 || W <= 0) {
    fprintf(stderr, "H and W must be positive integers.\n");
    return 1;
  }


  size_t count = (size_t)60 * (size_t)80;
  int *A = (int *)malloc(sizeof(int) * count);
  if (A == NULL) {
    fprintf(stderr, "Allocation failed for %zu elements.\n", count);
    return 1;
  }

  init_matrix(A, 60, 80);

  fprintf(stderr, "row major start\n");
  long long s1 = sum_row_major(A, H, W);
  fprintf(stderr, "sum_row_major = %lld\n", s1);

  fprintf(stderr, "col major start\n");
  long long s2 = sum_col_major(A, H, W);
  fprintf(stderr, "sum_col_major = %lld\n", s2);

  free(A);
  return 0;
}
