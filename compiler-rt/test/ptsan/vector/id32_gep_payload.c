//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Bounded vector offsets can keep their IDs, including when an intermediate
// pointer is stored and LLVM leaves a chain of vector GEPs. The proof must use
// the total offset from the allocation, not just the last GEP's displacement.
// Unknown allocation offsets and unbounded indices still require conversion.
// REQUIRES: x86_64-target-arch
// RUN: %clang_raw -O3 -march=x86-64 -fsanitize=ptsan -fsanitize-ptsan-32-bit-ids -mllvm -ptsan-stop-after=late-optimization -S -emit-llvm %s -o %t.ll
// RUN: FileCheck %s --input-file=%t.ll
// RUN: %clang_raw -O3 -march=haswell -fsanitize=ptsan -fsanitize-ptsan-32-bit-ids -mllvm -ptsan-stop-after=late-optimization -S -emit-llvm %s -o %t.slp.ll
// RUN: FileCheck %s --check-prefix=SPLAT --input-file=%t.slp.ll
// RUN: %clang_ptsan_id32 -O3 -march=x86-64 %s -o %t
// RUN: %run %t

#include "../Inputs/ptsan_test_interface.h"
#include <assert.h>

int objects[256];

// Expect both vector GEPs to stay tagged, without stripping: their total byte
// offsets from objects are 0..60 and 12..72, respectively.
// CHECK-LABEL: define {{.*}} @constant_step(
// CHECK-NOT: call <4 x ptr> @__ptsan_strip_id32
// CHECK: [[FIRST:%.*]] = getelementptr inbounds i32, {{.*}} <4 x i64> {{.*}} !ptsan.tagged
// CHECK-NOT: call <4 x ptr> @__ptsan_tag_id32
// CHECK: getelementptr inbounds i8, <4 x ptr> [[FIRST]], i64 12, {{.*}} !ptsan.tagged
// CHECK-NOT: call <4 x ptr> @__ptsan_tag_id32
// CHECK: ret void
__attribute__((noinline))
void constant_step(int **restrict first, int **restrict second,
                   const unsigned *restrict indices) {
#pragma clang loop vectorize(enable) vectorize_width(4) interleave_count(1) unroll(disable)
  for (unsigned i = 0; i < 64; ++i) {
    int *p = objects + (indices[i] & 15);
    first[i] = p;
    second[i] = p + 3;
  }
}

// Expect both vector GEPs to stay tagged, without stripping: the second bounded
// index extends the total byte-offset range from 0..60 to 0..88.
// CHECK-LABEL: define {{.*}} @varying_step(
// CHECK: [[FIRST:%.*]] = getelementptr inbounds i32, {{.*}} <4 x i64> {{.*}} !ptsan.tagged
// CHECK-NOT: call <4 x ptr> @__ptsan_tag_id32
// CHECK: getelementptr inbounds i32, <4 x ptr> [[FIRST]], <4 x i64> {{.*}} !ptsan.tagged
// CHECK-NOT: call <4 x ptr> @__ptsan_tag_id32
// CHECK: ret void
__attribute__((noinline))
void varying_step(int **restrict first, int **restrict second,
                  const unsigned *restrict indices,
                  const unsigned *restrict steps) {
#pragma clang loop vectorize(enable) vectorize_width(4) interleave_count(1) unroll(disable)
  for (unsigned i = 0; i < 64; ++i) {
    int *p = objects + (indices[i] & 15);
    first[i] = p;
    second[i] = p + (steps[i] & 7);
  }
}

// Expect the scalar and vector GEPs to stay tagged, without stripping. The
// scalar prefix contributes 0..60 bytes and the vector adds 0..28 bytes.
// CHECK-LABEL: define {{.*}} @scalar_start(
// CHECK-NOT: call <4 x ptr> @__ptsan_strip_id32
// CHECK: [[START:%.*]] = getelementptr inbounds i32, ptr {{.*}} !ptsan.tagged
// CHECK: getelementptr inbounds i32, ptr [[START]], <4 x i64> {{.*}} !ptsan.tagged
// CHECK-NOT: call <4 x ptr> @__ptsan_tag_id32
// CHECK: ret ptr
__attribute__((noinline))
int *scalar_start(unsigned index, int **restrict output,
                  const unsigned *restrict indices) {
  int *start = objects + (index & 15);
#pragma clang loop vectorize(enable) vectorize_width(4) interleave_count(1) unroll(disable)
  for (unsigned i = 0; i < 64; ++i) {
    output[i] = start + (indices[i] & 7);
  }
  return start;
}

// Expect the vector GEP to operate on a stripped pointer, then retag its result.
// The displacement is bounded, but base's offset within its object is unknown.
// CHECK-LABEL: define {{.*}} @unknown_base(
// CHECK: getelementptr {{.*}} <4 x i64> {{.*}} !ptsan.stripped
// CHECK: call <4 x ptr> @__ptsan_tag_id32
// CHECK: ret void
__attribute__((noinline))
void unknown_base(int *base, int **restrict output,
                  const unsigned *restrict indices) {
#pragma clang loop vectorize(enable) vectorize_width(4) interleave_count(1) unroll(disable)
  for (unsigned i = 0; i < 64; ++i) {
    output[i] = base + (indices[i] & 15);
  }
}

// Expect both vector GEPs to operate on stripped pointers, then retag their
// results. Even though the final step is small, the prefix can exceed the
// representable payload: an unrestricted unsigned index is scaled by four.
// CHECK-LABEL: define {{.*}} @unbounded_prefix(
// CHECK: [[FIRST:%.*]] = getelementptr inbounds i32, {{.*}} <4 x i64> {{.*}} !ptsan.stripped
// CHECK: getelementptr inbounds i8, <4 x ptr> [[FIRST]], i64 12, {{.*}} !ptsan.stripped
// CHECK: call <4 x ptr> @__ptsan_tag_id32
// CHECK: ret void
__attribute__((noinline))
void unbounded_prefix(int **restrict first, int **restrict second,
                      const unsigned *restrict indices) {
#pragma clang loop vectorize(enable) vectorize_width(4) interleave_count(1) unroll(disable)
  for (unsigned i = 0; i < 64; ++i) {
    int *p = objects + indices[i];
    first[i] = p;
    second[i] = p + 3;
  }
}

// Expect tagged vector arithmetic: scaling this index by four leaves the
// total byte offset below 2 GiB, regardless of the object's size.
// CHECK-LABEL: define {{.*}} @within_headroom(
// CHECK: getelementptr {{.*}} <4 x i64> {{.*}} !ptsan.tagged
// CHECK: ret void
__attribute__((noinline))
void within_headroom(int **restrict output, const unsigned *restrict indices) {
#pragma clang loop vectorize(enable) vectorize_width(4) interleave_count(1) unroll(disable)
  for (unsigned i = 0; i < 64; ++i) {
    output[i] = objects + (indices[i] & 0x1fffffffU);
  }
}

// Expect stripped vector arithmetic: scaling by four can reach 2 GiB,
// which is outside the guaranteed positive headroom.
// CHECK-LABEL: define {{.*}} @past_headroom(
// CHECK: getelementptr {{.*}} <4 x i64> {{.*}} !ptsan.stripped
// CHECK: call <4 x ptr> @__ptsan_tag_id32
// CHECK: ret void
__attribute__((noinline))
void past_headroom(int **restrict output, const unsigned *restrict indices) {
#pragma clang loop vectorize(enable) vectorize_width(4) interleave_count(1) unroll(disable)
  for (unsigned i = 0; i < 64; ++i) {
    output[i] = objects + (indices[i] & 0x20000000U);
  }
}

// Expect the vector GEP to stay tagged, without stripping. SLP broadcasts a
// scalar pointer through insertelement/shufflevector; the TaggedAnchor on the
// inserted scalar must not hide its allocation base or its scalar offset.
// This IR check uses AVX to make SLP choose four pointer lanes. Runtime checks
// still compile for baseline x86-64 and do not require AVX on the test host.
// SPLAT-LABEL: define {{.*}} @scalar_splat(
// SPLAT-NOT: call <4 x ptr> @__ptsan_strip_id32
// SPLAT: shufflevector <4 x ptr>
// SPLAT: getelementptr i8, <4 x ptr> {{.*}} !ptsan.tagged
// SPLAT-NOT: call <4 x ptr> @__ptsan_tag_id32
// SPLAT: ret void
__attribute__((noinline))
void scalar_splat(unsigned index, int **restrict output) {
  int *start = objects + (index & 15);
  output[0] = start + 1;
  output[1] = start + 3;
  output[2] = start + 5;
  output[3] = start + 7;
}

int main(void) {
  unsigned indices[64], steps[64];
  int *first[64], *second[64];
  PTSanID id = get_id(objects);
  for (unsigned i = 0; i < 64; ++i) {
    indices[i] = i & 15;
    steps[i] = i & 7;
  }
  constant_step(first, second, indices);
  for (unsigned i = 0; i < 64; ++i) {
    assert(first[i] == objects + indices[i]);
    assert(second[i] == objects + indices[i] + 3);
    assert(get_id(first[i]) == id && get_id(second[i]) == id);
  }
  varying_step(first, second, indices, steps);
  for (unsigned i = 0; i < 64; ++i) {
    assert(first[i] == objects + indices[i]);
    assert(second[i] == objects + indices[i] + steps[i]);
    assert(get_id(first[i]) == id && get_id(second[i]) == id);
  }
  assert(scalar_start(13, first, steps) == objects + 13);
  for (unsigned i = 0; i < 64; ++i) {
    assert(first[i] == objects + 13 + steps[i]);
    assert(get_id(first[i]) == id);
  }
  unknown_base(objects, first, indices);
  for (unsigned i = 0; i < 64; ++i) {
    assert(first[i] == objects + indices[i]);
    assert(get_id(first[i]) == id);
  }
  unbounded_prefix(first, second, indices);
  for (unsigned i = 0; i < 64; ++i) {
    assert(first[i] == objects + indices[i]);
    assert(second[i] == objects + indices[i] + 3);
  }
  within_headroom(first, indices);
  past_headroom(second, indices);
  for (unsigned i = 0; i < 64; ++i) {
    assert(first[i] == objects + indices[i]);
    assert(second[i] == objects);
    assert(get_id(first[i]) == id && get_id(second[i]) == id);
  }
  scalar_splat(13, first);
  for (unsigned i = 0; i < 4; ++i) {
    assert(first[i] == objects + 14 + 2 * i);
    assert(get_id(first[i]) == id);
  }
  return 0;
}
