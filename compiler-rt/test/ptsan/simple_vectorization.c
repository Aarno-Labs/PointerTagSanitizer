// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> err.txt 1> out.txt
// RUN: cat err.txt | FileCheck %s --check-prefix=ERR
// RUN: cat out.txt | FileCheck %s

// ERR: Unreclaimed IDs at exit: 0

// CHECK: sum = 523776

#include <stdio.h>
#include <stdlib.h>

#define N 1024

int main(void) {
  /* Allocate a writable buffer (so it carries a PTSan pointer-ID). */
  int *buf = (int *)malloc(N * sizeof(int));
  if (!buf) {
    perror("malloc failed");
    return 1;
  }

  /* Initialise the buffer with non-constant data that escapes. */
  for (int i = 0; i < N; ++i)
    buf[i] = i;

  long long sum = 0;

  /* ---- Hot loop: LLVM should auto-vectorise this reduction ---- */
  for (int i = 0; i < N; ++i)
    sum += buf[i];
  /* ------------------------------------------------------------- */

  /* Use the result so the loop cannot be optimised away. */
  printf("sum = %lld\n", sum);

  free(buf);
  return 0;
}
