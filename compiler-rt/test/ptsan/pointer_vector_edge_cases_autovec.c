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
// RUN: not ./%exe select-bad > select-bad-out.txt 2> select-bad-err.txt
// RUN: FileCheck %s --check-prefix=BAD < select-bad-err.txt
// RUN: not ./%exe recurrence-bad > recurrence-bad-out.txt 2> recurrence-bad-err.txt
// RUN: FileCheck %s --check-prefix=BAD < recurrence-bad-err.txt
// RUN: not ./%exe guarded-bad > guarded-bad-out.txt 2> guarded-bad-err.txt
// RUN: FileCheck %s --check-prefix=BAD < guarded-bad-err.txt
// RUN: not ./%exe nullable-bad > nullable-bad-out.txt 2> nullable-bad-err.txt
// RUN: FileCheck %s --check-prefix=BAD < nullable-bad-err.txt

// These source-level cases pin naturally generated pointer-vector shapes and
// their runtime semantics. The current global-hoisting cost model keeps the
// select and recurrence tagged and inserts exact-use strips; these checks do
// not claim that those two kernels force lane-wise GetID/Tag lowering.

// LATE-LABEL: define {{.*}} @mixed_select(
// LATE: [[COMMON:%.*]] = getelementptr inbounds i32, ptr %common, <4 x i64>
// LATE: [[BASES:%.*]] = load <4 x ptr>,
// LATE: [[MIXED:%.*]] = getelementptr inbounds i32, <4 x ptr> [[BASES]], <4 x i64>
// LATE: [[SELECTED:%.*]] = select <4 x i1> {{%.*}}, <4 x ptr> [[MIXED]], <4 x ptr> [[COMMON]]
// LATE: [[RAW_SELECTED:%.*]] = call <4 x ptr> @__ptsan_strip(<4 x ptr> [[SELECTED]])
// LATE: icmp eq <4 x ptr> [[RAW_SELECTED]],
// LATE: [[TAGGED_SELECTED:%.*]] = call <4 x ptr> @__ptsan_tagged(<4 x ptr> [[SELECTED]])
// LATE: store <4 x ptr> [[TAGGED_SELECTED]],

// LATE-LABEL: define {{.*}} @mixed_recurrence(
// LATE: [[RECURRENCE:%.*]] = phi <4 x ptr>
// LATE: [[BASES:%.*]] = load <4 x ptr>,
// LATE: [[CURRENT:%.*]] = getelementptr inbounds i32, <4 x ptr> [[BASES]], <4 x i64>
// LATE: [[RAW_CURRENT:%.*]] = call <4 x ptr> @__ptsan_strip(<4 x ptr> [[CURRENT]])
// LATE: shufflevector <4 x ptr> {{%.*}}, <4 x ptr> [[CURRENT]],
// LATE: icmp eq <4 x ptr> [[RAW_CURRENT]],

// LATE-LABEL: define {{.*}} @guarded_store(
// LATE: [[POINTERS:%.*]] = getelementptr inbounds i32, ptr %base, <4 x i64>
// LATE: [[TAGGED_POINTERS:%.*]] = call <4 x ptr> @__ptsan_tagged(<4 x ptr> [[POINTERS]])
// LATE: {{.*}}call void @llvm.masked.store.v4p0.p0(<4 x ptr> [[TAGGED_POINTERS]],

// LATE-LABEL: define {{.*}} @nullable_load(
// LATE: [[NULLS:%.*]] = call <4 x ptr> @__ptsan_tagged(<4 x ptr> zeroinitializer)
// LATE: [[LOADED:%.*]] = {{.*}}call <4 x ptr> @llvm.masked.load.v4p0.p0(ptr {{%.*}}, i32 8, <4 x i1> {{%.*}}, <4 x ptr> [[NULLS]])
// LATE: [[TAGGED_LOADED:%.*]] = call <4 x ptr> @__ptsan_tagged(<4 x ptr> [[LOADED]])
// LATE: store <4 x ptr> [[TAGGED_LOADED]],

// OUT: pointer-vector-edge-cases-ok
// ERR-NOT: Found bad
// ERR: Unreclaimed IDs at exit: 0
// ERR-NOT: Found bad

// Each bad mode forms a one-past pointer in the named vectorized kernel,
// then dereferences the transported pointer. Forming one-past is valid;
// the dereference must be rejected.
// BAD: Found bad POINT access

#include <stdio.h>
#include <stdlib.h>

#define N 67
#define OBJECT_SIZE 8
#define BAD_LANE 5

__attribute__((noinline)) void
mixed_select(int **restrict out, int *restrict equal, int *restrict common,
             int *const *restrict bases, const int *restrict common_indices,
             const int *restrict offsets, const int *restrict choose_common,
             int *const *restrict expected, int n) {
#pragma clang loop vectorize(enable) interleave(disable)
  for (int i = 0; i < n; ++i) {
    int *common_ptr = &common[common_indices[i]];
    int *mixed_ptr = bases[i] + offsets[i];
    int *selected = choose_common[i] ? common_ptr : mixed_ptr;
    equal[i] = selected == expected[i];
    out[i] = selected;
  }
}

__attribute__((noinline)) void
mixed_recurrence(int **restrict out, int *restrict equal,
                 int *const *restrict bases, const int *restrict offsets,
                 int *const *restrict expected, int *initial, int n) {
  int *previous = initial;
#pragma clang loop vectorize(enable) interleave(disable)
  for (int i = 0; i < n; ++i) {
    int *current = bases[i] + offsets[i];
    equal[i] = current == expected[i];
    out[i] = previous;
    previous = current;
  }
}

__attribute__((noinline)) void guarded_store(int **restrict out,
                                             int *restrict base,
                                             const int *restrict indices,
                                             int n) {
#pragma clang loop vectorize(enable) interleave(disable)
  for (int i = 0; i < n; ++i) {
    int j = indices[i];
    if (j >= 0)
      out[i] = &base[j];
  }
}

__attribute__((noinline)) void nullable_load(int **restrict out,
                                             int *const *restrict bases,
                                             const int *restrict use_null,
                                             int n) {
#pragma clang loop vectorize(enable) interleave(disable)
  for (int i = 0; i < n; ++i)
    out[i] = use_null[i] ? NULL : bases[i];
}

static int inactive_slot(int i) {
  int slot = i & 31;
  return slot == 0 || slot == 5 || slot == 10 || slot == 15 ||
         (slot >= 16 && slot < 20);
}

int main(int argc, char **argv) {
  char bad_mode = argc > 1 ? argv[1][0] : 0;
  int *common = malloc((N + OBJECT_SIZE) * sizeof(*common));
  int **bases = malloc(N * sizeof(*bases));
  int **expected = malloc(N * sizeof(*expected));
  int **out = malloc(N * sizeof(*out));
  int *indices = malloc(N * sizeof(*indices));
  int *offsets = malloc(N * sizeof(*offsets));
  int *flags = malloc(N * sizeof(*flags));
  int *equal = malloc(N * sizeof(*equal));
  if (!common || !bases || !expected || !out || !indices || !offsets ||
      !flags || !equal)
    return 1;

  for (int i = 0; i < N + OBJECT_SIZE; ++i)
    common[i] = 1000 + i;
  for (int i = 0; i < N; ++i) {
    bases[i] = malloc(OBJECT_SIZE * sizeof(**bases));
    if (!bases[i])
      return 2;
    for (int j = 0; j < OBJECT_SIZE; ++j)
      bases[i][j] = 2000 + 10 * i + j;
    indices[i] = i;
    offsets[i] = i % OBJECT_SIZE;
    flags[i] = i & 1;
    expected[i] = flags[i] ? common + indices[i] : bases[i] + offsets[i];
  }
  if (bad_mode == 's') {
    offsets[BAD_LANE] = OBJECT_SIZE;
    flags[BAD_LANE] = 0;
    expected[BAD_LANE] = bases[BAD_LANE] + offsets[BAD_LANE];
  }

  mixed_select(out, equal, common, bases, indices, offsets, flags, expected, N);
  if (bad_mode == 's')
    return *out[BAD_LANE];
  for (int i = 0; i < N; ++i)
    if (!equal[i] || out[i] != expected[i] ||
        *out[i] != (flags[i] ? 1000 + indices[i] : 2000 + 10 * i + offsets[i]))
      return 3;

  for (int i = 0; i < N; ++i) {
    offsets[i] = i % OBJECT_SIZE;
    expected[i] = bases[i] + offsets[i];
  }
  if (bad_mode == 'r') {
    offsets[BAD_LANE] = OBJECT_SIZE;
    expected[BAD_LANE] = bases[BAD_LANE] + offsets[BAD_LANE];
  }
  mixed_recurrence(out, equal, bases, offsets, expected, common + N, N);
  if (bad_mode == 'r')
    return *out[BAD_LANE + 1];
  for (int i = 0; i < N; ++i) {
    int *previous = i ? expected[i - 1] : common + N;
    if (!equal[i] || out[i] != previous || *out[i] != *previous)
      return 4;
  }

  for (int i = 0; i < N; ++i) {
    indices[i] = inactive_slot(i) ? -1 : i;
    out[i] = common + N;
  }
  if (bad_mode == 'g')
    indices[BAD_LANE] = N + OBJECT_SIZE;
  guarded_store(out, common, indices, N);
  if (bad_mode == 'g')
    return *out[BAD_LANE];
  for (int i = 0; i < N; ++i) {
    int *wanted = indices[i] < 0 ? common + N : common + indices[i];
    if (out[i] != wanted || *out[i] != *wanted)
      return 5;
  }

  for (int i = 0; i < N; ++i)
    flags[i] = inactive_slot(i);
  if (bad_mode == 'n') {
    bases[BAD_LANE] += OBJECT_SIZE;
    flags[BAD_LANE] = 0;
  }
  nullable_load(out, bases, flags, N);
  if (bad_mode == 'n')
    return *out[BAD_LANE];
  for (int i = 0; i < N; ++i) {
    if (flags[i]) {
      if (out[i] != NULL)
        return 6;
    } else if (out[i] != bases[i] || *out[i] != 2000 + 10 * i) {
      return 7;
    }
  }

  for (int i = 0; i < N; ++i)
    free(bases[i]);
  free(equal);
  free(flags);
  free(offsets);
  free(indices);
  free(out);
  free(expected);
  free(bases);
  free(common);
  puts("pointer-vector-edge-cases-ok");
  return 0;
}
