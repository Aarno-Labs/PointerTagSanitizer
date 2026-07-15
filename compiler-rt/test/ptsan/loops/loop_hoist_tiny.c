// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> err.txt
// RUN: ./%exe > out.txt
// RUN: cat err.txt | FileCheck %s --check-prefix=ERR
// RUN: cat out.txt | FileCheck %s --check-prefix=OUT

// RUN: %clang_ptsan -g -O2 -o out.ll -S -emit-llvm %s
// RUN cat out.ll | FileCheck %s --check-prefix=LL

// ERR-NOT: Found bad
// OUT: got 2
// Should have exactly one ptsan_complain
// LL: call void @ptsan_complain(i32 0
// LL-NOT: call void @ptsan_complain(i32 1

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

void __attribute__((noinline)) use(int *buf)
{
  printf("got %d\n", buf[1]);
}

int main(void) {
  const int order = 3; /* must be ≤ 12 for AVX2 path */

  /* Predictor coefficients: a simple ramp 1…8 */
  int qlp_coeff[order];
  for (unsigned i = 0; i < order; ++i)
    qlp_coeff[i] = i + 1;

  use(qlp_coeff);
}
