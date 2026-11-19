//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan_plain -fsanitize-ptsan-32-bit-ids -O1 -g -fno-discard-value-names -S -emit-llvm %s -o - | FileCheck %s --implicit-check-not=has.stack.id.room --implicit-check-not='call i32 @ptsan_refill_stack_origins' --implicit-check-not='load ptr, ptr @ptsan_stack_origin_next'

extern void escape(void *);

// A single over-aligned object sends every object through runtime allocation,
// origin selection, and retirement, including the ordinary-alignment object.
void aligned_fixed(void) {
  char ordinary[17];
  _Alignas(8192) char aligned[19];
  escape(ordinary);
  escape(aligned);
}

// CHECK-LABEL: define {{.*}}@aligned_fixed(
// CHECK: call void @ptsan_get_stack_ids
// CHECK-DAG: call i32 @ptsan_choose_stack_origin(i64 4096)
// CHECK-DAG: call i32 @ptsan_choose_stack_origin(i64 8192)
// CHECK: call void @escape
// CHECK: call void @ptsan_free_stack_ids
// CHECK: ret void

// Include a nonrepeating VLA whose bounds are initialized after entry.
void aligned_dynamic(unsigned n) {
  char ordinary[17];
  _Alignas(8192) char aligned[n];
  escape(ordinary);
  escape(aligned);
}

// CHECK-LABEL: define {{.*}}@aligned_dynamic(
// CHECK: call void @ptsan_get_stack_ids
// CHECK: call i32 @ptsan_choose_stack_origin(i64 4096)
// CHECK: call i32 @ptsan_choose_stack_origin(i64 8192)
// CHECK: call void @escape
// CHECK: call void @ptsan_free_stack_ids
// CHECK: ret void

// Repeated allocas already use runtime origins; they also disable fast IDs for
// the other objects when they request alignment above the cache's contract.
void aligned_repeated(unsigned n, unsigned count) {
  char ordinary[17];
  escape(ordinary);
  for (unsigned i = 0; i != count; ++i) {
    void *aligned = __builtin_alloca_with_align(n, 8192 * 8);
    escape(aligned);
  }
}

// CHECK-LABEL: define {{.*}}@aligned_repeated(
// CHECK: call void @ptsan_get_stack_ids
// CHECK: call i32 @ptsan_choose_stack_origin(i64 4096)
// CHECK: call void @escape
// CHECK-DAG: call {{.*}} @ptsan_get_repeatable_alloca_id
// CHECK-DAG: call void @ptsan_free_stack_ids
// CHECK: ret void
