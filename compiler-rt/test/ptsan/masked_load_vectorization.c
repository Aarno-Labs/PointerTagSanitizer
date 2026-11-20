// REQUIRES: x86_64-target-arch
// RUN: rm -rf %t && mkdir -p %t && cd %t

// RUN: %clang_ptsan -O2 -march=native -S -emit-llvm %s -o - | FileCheck %s

// Make sure we're actually generating masked loads
// CHECK: call <{{[0-9]+}} x i32> @llvm.masked.load
// CHECK: call void @llvm.masked.store

// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> err.txt 1> out.txt
// RUN: cat err.txt | FileCheck %s --check-prefix=ERR
// RUN: cat out.txt | FileCheck %s --check-prefix=OUT

// ERR: Unreclaimed IDs at exit: 0

// OUT: checksum = 130560


#include <stdio.h>
#include <stdlib.h>

#define N 1024

int main(void) {
  /* Allocate writable buffers so they receive a PTSan pointer-ID. */
  int *src  = (int *)malloc(N * sizeof(int));
  int *dst  = (int *)malloc(N * sizeof(int));
  unsigned char *mask = (unsigned char *)malloc(N);

  if (!src || !dst || !mask) {
    perror("malloc");
    return 1;
  }

  /* Fill them with non-constant data. */
  for (int i = 0; i < N; ++i) {
    src[i]  = i;
    dst[i]  = 0;
    mask[i] = (i & 3) == 0;          /* true every fourth element           */
  }

  /* -------------- Predicated copy / add loop -------------- */
  for (int i = 0; i < N; ++i)
    if (mask[i])                     /* condition -> vector predicate mask  */
      dst[i] += src[i];              /* needs masked.load / masked.store    */
  /* --------------------------------------------------------- */

  /* Consume *some* result so the loop isn’t dead-code-eliminated. */
  long long checksum = 0;
  for (int i = 0; i < N; ++i)
    checksum += dst[i];

  printf("checksum = %lld\n", checksum);

  free(src);
  free(dst);
  free(mask);
  return 0;
}
