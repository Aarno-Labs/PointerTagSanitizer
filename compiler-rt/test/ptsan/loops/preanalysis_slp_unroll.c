//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: %clang_raw -fsanitize=ptsan -O3 -march=x86-64 \
// RUN:   -fno-discard-value-names -mllvm -ptsan-runtime-validate \
// RUN:   -mllvm -ptsan-stop-after=optimization -S -emit-llvm -o %t.loop.ll %s
// RUN: FileCheck %s --check-prefixes=CHECK,LOOP < %t.loop.ll
// RUN: %clang_raw -fsanitize=ptsan -O3 -march=haswell -mno-bmi2 \
// RUN:   -fno-discard-value-names -mllvm -ptsan-runtime-validate \
// RUN:   -mllvm -print-before=slp-vectorizer -mllvm -print-after=slp-vectorizer \
// RUN:   -mllvm -filter-print-funcs=block_difference \
// RUN:   -mllvm -ptsan-stop-after=optimization -S -emit-llvm \
// RUN:   -o %t.unrolled.ll %s 2> %t.slp.txt
// RUN: FileCheck %s --check-prefix=SLP < %t.slp.txt
// RUN: FileCheck %s --check-prefixes=CHECK,UNROLLED < %t.unrolled.ll
// RUN: %clang_raw -fsanitize=ptsan -O3 -march=x86-64 \
// RUN:   -mllvm -ptsan-runtime-validate -mllvm -ptsan-abort-on-error \
// RUN:   -mllvm -ptsan-stack-temporal-safety=0 -o %t.loop %s
// RUN: %run %t.loop 0 2> %t.loop.good.txt
// RUN: FileCheck %s --check-prefix=GOOD --allow-empty < %t.loop.good.txt
// RUN: not %run %t.loop 1 2> %t.loop.bad.txt
// RUN: FileCheck %s --check-prefix=BAD < %t.loop.bad.txt
// RUN: %clang_raw -fsanitize=ptsan -O3 -march=haswell -mno-bmi2 \
// RUN:   -mllvm -ptsan-runtime-validate -mllvm -ptsan-abort-on-error \
// RUN:   -mllvm -ptsan-stack-temporal-safety=0 -o %t.unrolled %s
// RUN: %run %t.unrolled 0 2> %t.unrolled.good.txt
// RUN: FileCheck %s --check-prefix=GOOD --allow-empty < %t.unrolled.good.txt
// RUN: not %run %t.unrolled 1 2> %t.unrolled.bad.txt
// RUN: FileCheck %s --check-prefix=BAD < %t.unrolled.bad.txt

// Sum absolute byte differences between two 16-by-16 blocks with runtime row
// strides: a standalone model of the x264 SAD kernel, not benchmark source.
// Preanalysis successfully summarizes both blocks. SLP then replaces the 32
// scalar loads with two vector loads, which must retain ptsan.loop.ids. The
// generic target retains the row loop; Haswell fully unrolls it. Both now rely
// on instruction metadata for membership. Losing it in the unrolled case used
// to leave 32 ordinary checks instead of two whole-block prechecks. A fixed
// target makes this independent of the host's -march=native choice.
//
// Offset 0 reads both complete arrays; offset 1 reads one byte past a's end.
// Both builds must accept the former and report the latter: this is an
// optimization regression, not missing runtime protection.

// CHECK-LABEL: define {{.*}} @block_difference(
// CHECK: call void @llvm.sideeffect() {{.*}}!ptsan.loop.bounds
// CHECK-COUNT-2: call void (...) @__ptsan_loop_preanalysis(
// CHECK-NOT: call void (...) @__ptsan_loop_preanalysis(
// CHECK-NOT: call void (...) @__ptsan_access(
// LOOP: phi i32
// CHECK-NOT: call void (...) @__ptsan_access(
// LOOP-COUNT-2: load <16 x i8>, ptr
// UNROLLED-COUNT-32: load <16 x i8>, ptr
// CHECK-NOT: load <16 x i8>, ptr
// CHECK-NOT: call void (...) @__ptsan_access(
// CHECK: ret i32

// SLP-LABEL: IR Dump Before SLPVectorizerPass on block_difference
// SLP: call void @llvm.sideeffect() {{.*}}!ptsan.loop.bounds
// SLP: load i8, ptr {{.*}}!ptsan.loop.ids ![[IDS:[0-9]+]]
// SLP-COUNT-31: load i8, ptr {{.*}}!ptsan.loop.ids ![[IDS]]
// SLP: ret i32
// SLP-LABEL: IR Dump After SLPVectorizerPass on block_difference
// SLP: call void @llvm.sideeffect() {{.*}}!ptsan.loop.bounds
// SLP-COUNT-2: load <16 x i8>, ptr {{.*}}!ptsan.loop.ids ![[IDS]]
// SLP: ret i32

// GOOD-NOT: Found bad
// BAD: Found bad PREANALYZED_LOOP access

#include <stdlib.h>

__attribute__((noinline))
int block_difference(const unsigned char *a, int a_stride,
                     const unsigned char *b, int b_stride) {
  int sum = 0;
  for (int row = 0; row < 16; ++row) {
    for (int column = 0; column < 16; ++column) {
      sum += abs((int)a[column] - (int)b[column]);
    }
    a += a_stride;
    b += b_stride;
  }
  return sum;
}

int main(int argc, char **argv) {
  unsigned char a[256] = {1};
  unsigned char b[256] = {0};
  int offset = argc > 1 ? atoi(argv[1]) : 0;
  volatile int result = block_difference(a + offset, 16, b, 16);
  (void)result;
  return 0;
}
