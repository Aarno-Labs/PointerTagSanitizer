//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-stack-temporal-safety=1 -mllvm -ptsan-random-seed=1 %s -o %t
// RUN: %run %t
// RUN: not %run %t reuse 2>&1 | FileCheck %s --check-prefix=UAF
// UAF: Found bad POINT access

#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <setjmp.h>

#define RAW __attribute__((noinline, disable_sanitizer_instrumentation))
extern __thread uint32_t *ptsan_stack_origin_next;
extern uint32_t ptsan_refill_stack_origins(void);
extern struct { uint64_t bias, span; } *ptsan_bounds;

static PTSanID ids[3];
static uint32_t origins[3];
static uint64_t biases[3], addresses[3];
static char *escaped;
static volatile char sink;
static jmp_buf env;

static RAW uint32_t *cursor(void) { return ptsan_stack_origin_next; }

// Inspect native runtime metadata without registering additional stack objects.
static RAW void observe(char *a, char *b, char *c) {
  const char *p[] = {a, b, c};
  for (unsigned i = 0; i < 3; ++i) {
    ids[i] = get_id(p[i]);
    origins[i] = ptsan_get_origin(ids[i]);
    addresses[i] = ptsan_get_address(p[i]);
    biases[i] = ptsan_bounds[ids[i]].bias;
    assert(ptsan_get_size(ids[i]) != 0);
  }
  assert((origins[0] & ~4095U) == (origins[1] & ~4095U));
  assert((origins[0] & ~4095U) == (origins[2] & ~4095U));
  for (unsigned i = 0; i < 3; ++i) {
    unsigned mask = 4095;
    assert((origins[i] & ~mask) != 0);
    assert((origins[i] & mask) == (addresses[i] & mask));
  }
}

__attribute__((noinline)) static void frame(int jump, int use_old) {
  char a[17], b[31];
  _Alignas(4096) char c[19];
  a[0] = b[0] = c[0] = 42;
  observe(a, b, c);
  if (use_old)
    sink = *escaped;
  escaped = a;
  if (jump)
    longjmp(env, 1);
  __asm__ volatile("" : : "r"(a), "r"(b), "r"(c) : "memory");
}

static PTSanID slow_ids[2];

static RAW void observe_slow(char *ordinary, char *aligned) {
  slow_ids[0] = get_id(ordinary);
  slow_ids[1] = get_id(aligned);
  assert(ptsan_get_size(slow_ids[0]) == 17);
  assert(ptsan_get_size(slow_ids[1]) == 19);
  assert(ptsan_get_origin(slow_ids[0]) >= 4096);
  assert(ptsan_get_origin(slow_ids[1]) >= 16384);
  assert(((uintptr_t)aligned & 16383) == 0);
  assert((ptsan_get_address(aligned) & 16383) == 0);
}

__attribute__((noinline)) static void overaligned_frame(void) {
  char ordinary[17];
  _Alignas(16384) char aligned[19];
  observe_slow(ordinary, aligned);
}

static RAW void retired(void) {
  for (unsigned i = 0; i < 3; ++i) {
    assert(ptsan_get_size(ids[i]) == 0);
    assert(ptsan_bounds[ids[i]].bias == biases[i]);
    assert(ptsan_get_origin(ids[i]) == origins[i]);
  }
  assert(ptsan_get_address(escaped) == addresses[0]);
}

static RAW void refill_test(void) {
  uint32_t salt = ptsan_refill_stack_origins();
  assert(salt >= 4096 && salt < (1U << 31) && (salt & 4095) == 0);
  uint32_t *begin = cursor();
  // The returned salt is separate: all 1024 buffered salts remain available.
  for (unsigned i = 0; i < 1024; ++i) {
    assert(cursor() == begin + i);
    uint32_t expected = *cursor();
    assert(expected >= 4096 && expected < (1U << 31));
    assert((expected & 4095) == 0);
    frame(0, 0);
    assert(cursor() == begin + i + 1);
    assert((origins[0] & ~4095U) == expected);
    retired();
  }
  assert(*cursor() == 0);
  // A generated draw at the sentinel refills without consuming cache[0].
  frame(0, 0);
  assert(cursor() == begin);
  assert(begin[1024] == 0);
  retired();
  uint32_t expected = *begin;
  frame(0, 0);
  assert(cursor() == begin + 1);
  assert((origins[0] & ~4095U) == expected);
  retired();
}

// setjmp must restore IDs and invalidate spans without restoring the salt cursor.
__attribute__((noinline)) static void jump_test(void) {
  if (!setjmp(env))
    frame(1, 0);
}

int RAW main(int argc, char **argv) {
  uint64_t start = ptsan_get_stack_cursor();
  frame(0, 0);
  retired();
  PTSanID first_id = ids[0];
  uint32_t first_origin = origins[0];
  if (argc > 1) {
    frame(0, 1);
    return 1;
  }

  refill_test();

  // Thousands of frames cross multiple buffer refills; IDs are reused promptly.
  unsigned changed = 0;
  for (unsigned i = 0; i < 4096; ++i) {
    uint32_t *before = cursor();
    frame(0, 0);
    assert(ptsan_get_stack_cursor() == start);
    assert(ids[0] == first_id);
    changed += origins[0] != first_origin;
    // Normal draws advance one word regardless of the number of objects.
    if (i == 0)
      assert(cursor() == before + 1);
    retired();
  }
  assert(changed > 4000);

  // Leave ample room so this check does not cross a refill boundary.
  while (*ptsan_stack_origin_next)
    frame(0, 0);
  frame(0, 0);
  uint32_t *before = cursor();
  jump_test();
  assert(cursor() == before + 1);
  assert(ptsan_get_stack_cursor() == start);
  retired();
  frame(0, 0);
  retired();

  // Over-alignment sends the whole frame through the existing slow path.
  // It must preserve alignment, retire both objects, and leave the salt cache
  // untouched for the next ordinary frame.
  before = cursor();
  overaligned_frame();
  assert(cursor() == before);
  assert(ptsan_get_stack_cursor() == start);
  assert(ptsan_get_size(slow_ids[0]) == 0);
  assert(ptsan_get_size(slow_ids[1]) == 0);
  frame(0, 0);
  assert(cursor() == before + 1);
  retired();
  return 0;
}
