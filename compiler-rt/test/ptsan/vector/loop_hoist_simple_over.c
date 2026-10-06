//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -O2 -march=skylake-avx512 -fno-discard-value-names \
// RUN:   -mllvm -force-vector-width=4 -mllvm -force-vector-interleave=1 \
// RUN:   -S -emit-llvm -o ptsan.ll %s
// RUN: FileCheck %s --check-prefix=IR < ptsan.ll
// RUN: %clang_ptsan -O2 -march=skylake-avx512 \
// RUN:   -mllvm -force-vector-width=4 -mllvm -force-vector-interleave=1 \
// RUN:   -S -emit-llvm -o runtime.ll %s
// Keep the vector IR and PTSan instrumentation, but execute baseline x86-64.
// Disable LLVM passes in the second compile to avoid reinstrumenting the IR.
// RUN: sed -E 's/"(target-cpu|target-features|tune-cpu)"="[^"]*"//g' runtime.ll | \
// RUN:   %clang_ptsan -O2 -march=x86-64 -Xclang -disable-llvm-passes -x ir -o %exe -
// RUN: ./%exe 0 2>good.txt
// RUN: FileCheck %s --check-prefix=GOOD < good.txt
// RUN: ./%exe 4096 2>good.txt
// RUN: FileCheck %s --check-prefix=GOOD < good.txt
// RUN: ./%exe 4100 2>bad.txt
// RUN: FileCheck %s --check-prefix=BAD < bad.txt

// This is the affine-scatter counterpart of loops/loop_hoist_simple_over.c.
// The stride is large enough to select llvm.masked.scatter, while the address
// remains an affine function of the induction variable so the existing loop
// range analysis can summarize and hoist the check.
//
// Length 4100 puts four invalid accesses in a vector iteration. It avoids
// accidentally testing only the scalar remainder after length 4097.
//
// IR-LABEL: define {{.*}} @scatter_loop(
// IR: call void @ptsan_complain(
// IR-NOT: call void @ptsan_complain_vector(
// IR: {{.*}}call void @llvm.masked.scatter.v4f32.v4p0(
// IR-NOT: call void @ptsan_complain{{(_vector)?}}(
// IR: ret void

// GOOD-NOT: Found bad
// GOOD: Total checks called: {{[1-5]}}
// BAD: Found bad PREANALYZED_LOOP access

#include <stdio.h>
#include <stdlib.h>

#define LOGICAL_BUFFER_SIZE 4096
#define STRIDE 17
#define BUFFER_SIZE (LOGICAL_BUFFER_SIZE * STRIDE)
#define MAX_LENGTH 4300

__attribute__((noinline)) void scatter_loop(float *restrict buffer,
                                            int length) {
#pragma clang loop vectorize(enable) vectorize_width(4) interleave_count(1)
  for (int i = 0; i < length; ++i)
    buffer[i * STRIDE] = 1.0f;
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 1;

  int length = atoi(argv[1]);
  if (length < 0 || length > MAX_LENGTH)
    return 1;

  float *buffer = (float *)malloc(BUFFER_SIZE * sizeof(*buffer));
  if (!buffer)
    return 1;

  scatter_loop(buffer, length);

  free(buffer);
  return 0;
}
