//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_raw -fsanitize=ptsan -O2 -mavx2 \
// RUN:   -fno-discard-value-names -mllvm -force-vector-width=4 \
// RUN:   -mllvm -force-vector-interleave=1 -mllvm -ptsan-nochecks \
// RUN:   -mllvm -ptsan-stop-after=late-optimization -S -emit-llvm \
// RUN:   -o late.ll %s
// RUN: FileCheck %s --check-prefix=LATE < late.ll
// RUN: %clang_ptsan -O2 -mavx2 -mllvm -force-vector-width=4 \
// RUN:   -mllvm -force-vector-interleave=1 -mllvm -ptsan-abort-on-error \
// RUN:   -o %exe %s
// RUN: ./%exe > out.txt 2> err.txt
// RUN: FileCheck %s --check-prefix=OUT < out.txt
// RUN: FileCheck %s --check-prefix=ERR < err.txt
// RUN: not ./%exe common-bad > common-bad-out.txt 2> common-bad-err.txt
// RUN: FileCheck %s --check-prefix=BAD < common-bad-err.txt
// RUN: not ./%exe mixed-bad > mixed-bad-out.txt 2> mixed-bad-err.txt
// RUN: FileCheck %s --check-prefix=BAD < mixed-bad-err.txt

// LATE-LABEL: define {{.*}} @common_ids(
// LATE: [[COMMON:%.*]] = getelementptr inbounds i32, ptr %base, <4 x i64>
// LATE: [[COMMON_RAW:%.*]] = call <4 x ptr> @__ptsan_strip(<4 x ptr> [[COMMON]])
// LATE: icmp eq <4 x ptr> [[COMMON_RAW]],
// LATE: [[COMMON_TAGGED:%.*]] = call <4 x ptr> @__ptsan_tagged(<4 x ptr> [[COMMON]])
// LATE: store <4 x ptr> [[COMMON_TAGGED]],

// LATE-LABEL: define {{.*}} @mixed_ids(
// LATE: [[BASES:%.*]] = load <4 x ptr>,
// LATE: [[MIXED:%.*]] = getelementptr inbounds i32, <4 x ptr> [[BASES]], <4 x i64>
// LATE: [[MIXED_RAW:%.*]] = call <4 x ptr> @__ptsan_strip(<4 x ptr> [[MIXED]])
// LATE: icmp eq <4 x ptr> [[MIXED_RAW]],
// LATE: [[MIXED_TAGGED:%.*]] = call <4 x ptr> @__ptsan_tagged(<4 x ptr> [[MIXED]])
// LATE: store <4 x ptr> [[MIXED_TAGGED]],

// LATE-LABEL: define {{.*}} @gep_recurrence(
// LATE: [[CURRENT:%.*]] = getelementptr inbounds i32, ptr %base, <4 x i64>
// LATE: [[CURRENT_RAW:%.*]] = call <4 x ptr> @__ptsan_strip(<4 x ptr> [[CURRENT]])
// LATE: shufflevector <4 x ptr> {{%.*}}, <4 x ptr> [[CURRENT]],
// LATE: icmp eq <4 x ptr> [[CURRENT_RAW]],

// OUT: pointer-vector-ids-ok
// ERR-NOT: Found bad
// ERR: Unreclaimed IDs at exit: 0
// ERR-NOT: Found bad

// The bad modes form one-past pointers in a vectorized kernel, then
// dereference the transported pointer. Forming one-past is valid; the
// dereference must be rejected.
// BAD: Found bad POINT access

#include <stdio.h>
#include <stdlib.h>

#define N 67
#define OBJECT_SIZE 8
#define BAD_LANE 5

__attribute__((noinline)) void
common_ids(int **restrict out, int *restrict equal, int *restrict base,
           const int *restrict indices, int *const *restrict expected, int n) {
#pragma clang loop vectorize(enable) interleave(disable)
  for (int i = 0; i < n; ++i) {
    int *p = &base[indices[i]];
    equal[i] = p == expected[i];
    out[i] = p;
  }
}

__attribute__((noinline)) void
mixed_ids(int **restrict out, int *restrict equal, int *const *restrict bases,
          const int *restrict offsets, int *const *restrict expected, int n) {
#pragma clang loop vectorize(enable) interleave(disable)
  for (int i = 0; i < n; ++i) {
    int *p = bases[i] + offsets[i];
    equal[i] = p == expected[i];
    out[i] = p;
  }
}

__attribute__((noinline)) void
gep_recurrence(int **restrict out, int *restrict equal, int *restrict base,
               const int *restrict indices, int *const *restrict expected,
               int *initial, int n) {
  int *previous = initial;
#pragma clang loop vectorize(enable) interleave(disable)
  for (int i = 0; i < n; ++i) {
    int *current = &base[indices[i]];
    equal[i] = current == expected[i];
    out[i] = previous;
    previous = current;
  }
}

int main(int argc, char **argv) {
  char bad_mode = argc > 1 ? argv[1][0] : 0;
  int *common = malloc((N + OBJECT_SIZE) * sizeof(*common));
  int **bases = malloc(N * sizeof(*bases));
  int **expected = malloc(N * sizeof(*expected));
  int **out = malloc(N * sizeof(*out));
  int *indices = malloc(N * sizeof(*indices));
  int *equal = malloc(N * sizeof(*equal));
  if (!common || !bases || !expected || !out || !indices || !equal)
    return 1;

  for (int i = 0; i < N + OBJECT_SIZE; ++i)
    common[i] = 1000 + i;
  for (int i = 0; i < N; ++i) {
    indices[i] = i;
    expected[i] = common + i;
  }
  if (bad_mode == 'c') {
    indices[BAD_LANE] = N + OBJECT_SIZE;
    expected[BAD_LANE] = common + indices[BAD_LANE];
  }

  common_ids(out, equal, common, indices, expected, N);
  if (bad_mode == 'c')
    return *out[BAD_LANE];
  for (int i = 0; i < N; ++i)
    if (!equal[i] || out[i] != common + i || *out[i] != 1000 + i)
      return 2;

  gep_recurrence(out, equal, common, indices, expected, common + N, N);
  for (int i = 0; i < N; ++i) {
    int *previous = i ? common + indices[i - 1] : common + N;
    if (!equal[i] || out[i] != previous || *out[i] != *previous)
      return 3;
  }

  for (int i = 0; i < N; ++i) {
    bases[i] = malloc(OBJECT_SIZE * sizeof(**bases));
    if (!bases[i])
      return 4;
    for (int j = 0; j < OBJECT_SIZE; ++j)
      bases[i][j] = 2000 + 10 * i + j;
    indices[i] = i % OBJECT_SIZE;
    expected[i] = bases[i] + indices[i];
  }
  if (bad_mode == 'm') {
    indices[BAD_LANE] = OBJECT_SIZE;
    expected[BAD_LANE] = bases[BAD_LANE] + indices[BAD_LANE];
  }

  mixed_ids(out, equal, bases, indices, expected, N);
  if (bad_mode == 'm')
    return *out[BAD_LANE];
  for (int i = 0; i < N; ++i)
    if (!equal[i] || out[i] != expected[i] ||
        *out[i] != 2000 + 10 * i + indices[i])
      return 5;

  for (int i = 0; i < N; ++i)
    free(bases[i]);
  free(equal);
  free(indices);
  free(out);
  free(expected);
  free(bases);
  free(common);
  puts("pointer-vector-ids-ok");
  return 0;
}
