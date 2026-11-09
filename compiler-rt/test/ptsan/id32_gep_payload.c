//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// A GEP carrying into the ID can turn an invalid pointer into a pointer to a
// different live allocation. Keep advance in another translation unit: its
// caller must not be able to recover the original ID by inlining the arithmetic.
// Also check that proven payloads can still use tagged arithmetic.
// RUN: split-file %s %t
// RUN: %clang_ptsan_id32 -O2 -I %S/Inputs %t/main.c %t/advance.c -o %t/test
// RUN: %run %t/test
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-no-opts -I %S/Inputs %t/main.c %t/advance.c -o %t/noopts
// RUN: %run %t/noopts
// RUN: %clang_raw -O2 -fno-discard-value-names -fsanitize=ptsan -fsanitize-ptsan-32-bit-ids -mllvm -ptsan-stop-after=late-optimization -S -emit-llvm %t/advance.c -o %t/anchors.ll
// RUN: FileCheck %s --input-file=%t/anchors.ll
// RUN: %clang_raw -O2 -fno-strict-overflow -fno-discard-value-names -fsanitize=ptsan -fsanitize-ptsan-32-bit-ids -mllvm -ptsan-stop-after=late-optimization -S -emit-llvm %t/advance.c -o %t/noninbounds.ll
// RUN: FileCheck %s --input-file=%t/noninbounds.ll
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-global-section=gep_payload %t/section.c -o %t/section
// RUN: %run %t/section
// RUN: %clang_raw -O2 -fsanitize=ptsan -fsanitize-ptsan-32-bit-ids -mllvm -ptsan-global-section=gep_payload -mllvm -ptsan-stop-after=late-optimization -S -emit-llvm %t/section.c -o %t/section.ll
// RUN: FileCheck %s --check-prefix=SECTION --input-file=%t/section.ll

// CHECK-LABEL: define {{.*}} @advance(
// CHECK: call ptr @__ptsan_strip_id32
// CHECK: getelementptr {{.*}} !ptsan.stripped
// CHECK: call ptr @__ptsan_tag_id32
// CHECK-LABEL: define {{.*}} @advance_one(
// CHECK: call ptr @__ptsan_strip_id32
// CHECK: getelementptr {{.*}} !ptsan.stripped
// CHECK: call ptr @__ptsan_tag_id32
// CHECK-LABEL: define {{.*}} @unbounded_global(
// CHECK: getelementptr {{.*}} !ptsan.stripped
// CHECK: call ptr @__ptsan_tag_id32
// CHECK-LABEL: define {{.*}} @bounded_global(
// CHECK: getelementptr {{.*}} !ptsan.tagged
// CHECK-NOT: call ptr @__ptsan_tag_id32
// CHECK: ret ptr
// CHECK-LABEL: define {{.*}} @payload_limit(
// CHECK: getelementptr {{.*}} !ptsan.tagged
// CHECK-NOT: call ptr @__ptsan_tag_id32
// CHECK: ret ptr
// CHECK-LABEL: define {{.*}} @past_payload_limit(
// CHECK: getelementptr {{.*}} !ptsan.stripped
// CHECK: call ptr @__ptsan_tag_id32
// CHECK-LABEL: define {{.*}} @within_global(
// CHECK-NOT: call ptr @__ptsan_strip
// CHECK: getelementptr {{.*}} !ptsan.tagged
// CHECK-NOT: call ptr @__ptsan_tag_id32
// CHECK: ret ptr
// CHECK-LABEL: define {{.*}} @before_global(
// CHECK: getelementptr {{.*}} !ptsan.stripped
// CHECK: call ptr @__ptsan_tag_id32
// CHECK: ret ptr
// CHECK-LABEL: define {{.*}} @bounded_stack(
// CHECK: getelementptr {{.*}} !ptsan.tagged
// CHECK: call void @consume
// CHECK-LABEL: define {{.*}} @bounded_dynamic_stack(
// CHECK: getelementptr {{.*}} !ptsan.tagged
// CHECK: call void @consume
// CHECK-LABEL: define {{.*}} @bounded_heap(
// CHECK: call {{.*}} @malloc
// CHECK: getelementptr {{.*}} !ptsan.tagged
// CHECK: call void @consume
// CHECK-LABEL: define {{.*}} @fixed_heap(
// CHECK: call {{.*}} @malloc
// CHECK: getelementptr {{.*}} !ptsan.tagged
// CHECK: call void @consume

// SECTION-LABEL: define {{.*}} @section_start(
// SECTION: getelementptr {{.*}} !ptsan.tagged
// SECTION-NOT: call ptr @__ptsan_tag_id32
// SECTION: ret ptr
// SECTION-LABEL: define {{.*}} @section_stop(
// SECTION: getelementptr {{.*}} !ptsan.stripped
// SECTION: call ptr @__ptsan_tag_id32

//--- main.c
#include "ptsan_test_interface.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

char first[32];
char second[32];
char *advance(char *p, ptrdiff_t delta);
char *advance_one(char *p);
char *bounded_global(unsigned index);
char *payload_limit(unsigned index);
char *past_payload_limit(unsigned index);
char *within_global(unsigned index);
char *before_global(unsigned index);
void bounded_stack(unsigned index);
void bounded_dynamic_stack(size_t size, unsigned index);
void bounded_heap(size_t size, unsigned index);
void fixed_heap(unsigned index);

void consume(char *p) { *p = 42; }

int main(void) {
  PTSanID id = get_id(first);
  ptrdiff_t origin = ptsan_get_origin(id);
  ptrdiff_t max_offset = UINT32_MAX - origin;
  assert(id != 0 && id != get_id(second));
  char *interior = advance(first, 7);
  assert(get_id(interior) == id);
  assert(get_id(advance(interior, -7)) == id);
  assert(get_id(advance(first, sizeof(first))) == id);
  assert(get_id(advance(first, max_offset)) == id);
  assert(get_id(advance(first, -origin)) == id);

  // These must not carry or borrow into a neighboring ID. Non-strict mode
  // returns the raw address when the payload cannot be represented.
  assert(get_id(advance(first, (ptrdiff_t)UINT32_MAX + 1)) == 0);
  assert(get_id(advance_one(advance(first, max_offset))) == 0);
  assert(get_id(advance(interior, max_offset - 6)) == 0);
  assert(get_id(advance(first, -origin - 1)) == 0);
  assert(get_id(advance(second, -((ptrdiff_t)UINT32_MAX + 1))) == 0);

  *bounded_global(5) = 11;
  assert(first[5] == 11);
  assert(get_id(payload_limit(UINT32_MAX)) == id);
  assert(get_id(past_payload_limit(0)) == id);
  *within_global(6) = 22;
  assert(first[6] == 22);
  assert(get_id(before_global(15)) == id);
  bounded_stack(9);
  bounded_dynamic_stack(32, 9);
  bounded_heap(32, 9);
  fixed_heap(9);
  return 0;
}

//--- advance.c
#include <stddef.h>
#include <stdlib.h>

extern char first[32];
void consume(char *p);

char *advance(char *p, ptrdiff_t delta) { return p + delta; }

// Even a one-byte displacement can overflow an unknown incoming payload.
char *advance_one(char *p) { return p + 1; }

char *unbounded_global(ptrdiff_t delta) { return first + delta; }

// Expect tagged arithmetic: every origin leaves room for this positive offset,
// even though the range exceeds the object's size.
char *bounded_global(unsigned index) { return first + (index & 65535); }

// Expect tagged arithmetic through the last byte of guaranteed headroom.
char *payload_limit(unsigned index) { return first + (index & 0x7fffffffU); }

// Expect stripped arithmetic: an offset of 2 GiB can carry out of the payload
// when the origin is 2 GiB.
char *past_payload_limit(unsigned index) { return first + (index & 0x80000000U); }

// Expect tagged arithmetic for the positive offset, but stripped arithmetic
// for the negative one. The proof does not assume negative-offset headroom.
char *within_global(unsigned index) { return first + (index & 15); }
char *before_global(unsigned index) { return first - (index & 15); }

void bounded_stack(unsigned index) {
  char buffer[32];
  consume(buffer + (index & 15));
}

// Expect tagged arithmetic: a dynamic size does not affect positive headroom.
void bounded_dynamic_stack(size_t size, unsigned index) {
  char buffer[size];
  consume(buffer + (index & 15));
}

// Expect tagged arithmetic for a known allocator, even with a dynamic size.
void bounded_heap(size_t size, unsigned index) {
  char *buffer = malloc(size);
  if (!buffer) {
    abort();
  }
  consume(buffer + (index & 15));
  free(buffer);
}

void fixed_heap(unsigned index) {
  char *buffer = malloc(32);
  if (!buffer) {
    abort();
  }
  consume(buffer + (index & 15));
  free(buffer);
}

//--- section.c
#include <assert.h>

char section_data[32] __attribute__((section("gep_payload"))) = {
    [5] = 11, [31] = 22};
extern const char __start_gep_payload[];
extern const char __stop_gep_payload[];

// Expect tagged arithmetic: the start names the object base, and the proof
// does not need the section's size.
__attribute__((noinline)) const char *section_start(unsigned index) {
  return __start_gep_payload + (index & 15);
}

// Expect stripped arithmetic: the stop has the section's ID, but its payload
// includes the unknown section size.
__attribute__((noinline)) const char *section_stop(unsigned index) {
  return __stop_gep_payload - (index & 15);
}

int main(void) {
  assert(*section_start(5) == 11);
  assert(*section_stop(1) == 22);
  return 0;
}
