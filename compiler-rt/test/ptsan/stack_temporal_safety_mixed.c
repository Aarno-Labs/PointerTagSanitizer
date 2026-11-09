//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Stack temporal safety is a per-translation-unit policy. Exercise alternating
// strong/weak frames in both directions, repeated dynamic allocas, deferred ID16
// retirement, bucket overflow, and longjmp across mixed frames. A weak reader
// must still diagnose a strong frame's retired object.
// RUN: %clang_ptsan -O2 -mllvm -ptsan-id-bucket-size=64 -mllvm -ptsan-stack-temporal-safety=1 -DFRAME_KIND=1 -c %s -o %t.id16.strong.o
// RUN: %clang_ptsan -O2 -mllvm -ptsan-id-bucket-size=64 -mllvm -ptsan-stack-temporal-safety=0 -DFRAME_KIND=0 -c %s -o %t.id16.weak.o
// RUN: %clang_ptsan -O2 -mllvm -ptsan-id-bucket-size=64 -mllvm -ptsan-stack-temporal-safety=1 %s %t.id16.strong.o %t.id16.weak.o -o %t.id16
// RUN: %run %t.id16 2>&1 | FileCheck %s --check-prefix=PASS
// RUN: not %run %t.id16 uar 2>&1 | FileCheck %s --check-prefix=UAR
//
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-id-bucket-size=64 -mllvm -ptsan-stack-temporal-safety=1 -DFRAME_KIND=1 -c %s -o %t.id32.strong.o
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-id-bucket-size=64 -mllvm -ptsan-stack-temporal-safety=0 -DFRAME_KIND=0 -c %s -o %t.id32.weak.o
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-id-bucket-size=64 -mllvm -ptsan-stack-temporal-safety=1 %s %t.id32.strong.o %t.id32.weak.o -o %t.id32
// RUN: %run %t.id32 2>&1 | FileCheck %s --check-prefix=PASS
// RUN: not %run %t.id32 uar 2>&1 | FileCheck %s --check-prefix=UAR

// PASS-NOT: Found bad
// PASS: mixed-stack-temporal-ok
// PASS-NOT: Found bad
// UAR: Found bad {{.*}} access

#include <alloca.h>
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include "Inputs/ptsan_test_interface.h"

extern jmp_buf destination;
extern volatile unsigned allocation_size;
extern char *escaped[2];
extern PTSanID escaped_ids[2];
void strong_frame(char *, unsigned, unsigned, int);
void weak_frame(char *, unsigned, unsigned, int);
void fill(char *, unsigned, unsigned);
void check(const char *, unsigned, unsigned);
unsigned weak_read(const char *);

#ifdef FRAME_KIND
#if FRAME_KIND
#define THIS_FRAME strong_frame
#define NEXT_FRAME weak_frame
#else
#define THIS_FRAME weak_frame
#define NEXT_FRAME strong_frame
__attribute__((noinline)) unsigned weak_read(const char *p) {
  return *(const volatile unsigned char *)p;
}
#endif

__attribute__((noinline))
void THIS_FRAME(char *parent, unsigned parent_value, unsigned depth, int jump) {
  char local[33];
  unsigned value = depth + 10;
  fill(local, sizeof(local), value);
  if (parent)
    check(parent, 33, parent_value);

  // Repeated dynamic allocations use the runtime log even on fast fixed frames.
  for (unsigned i = 0; i != 3; ++i) {
    unsigned n = allocation_size;
    char *dynamic = (char *)alloca(n);
    fill(dynamic, n, value + i);
    if (i == 2) {
      escaped[FRAME_KIND] = local;
      escaped_ids[FRAME_KIND] = get_id(local);
      if (depth)
        NEXT_FRAME(local, value, depth - 1, jump);
      else if (jump)
        longjmp(destination, 42);
    }
    check(dynamic, n, value + i);
    check(local, sizeof(local), value);
  }
  if (parent)
    check(parent, 33, parent_value);
}
#else
jmp_buf destination;
volatile unsigned allocation_size = 37;
char *escaped[2];
PTSanID escaped_ids[2];

__attribute__((noinline)) void fill(char *p, unsigned n, unsigned value) {
  p[0] = value;
  p[n - 1] = value;
  __asm__ volatile("" : : "r"(p) : "memory");
}

__attribute__((noinline)) void check(const char *p, unsigned n, unsigned value) {
  assert((unsigned char)p[0] == value);
  assert((unsigned char)p[n - 1] == value);
  __asm__ volatile("" : : "r"(p) : "memory");
}

// Runtime bookkeeping is deliberately untagged; inspect it without instrumenting
// this helper, just as the runtime does.
__attribute__((noinline, disable_sanitizer_instrumentation))
static uintptr_t log_cursor(void) { return ptsan_stack_id_log.cursor; }

int main(int argc, char **argv) {
  if (argc > 1) {
    // A weak reader still detects a strong frame's retired stack object.
    strong_frame(0, 0, 0, 0);
    return weak_read(escaped[1]);
  }
  char parent[33];
  fill(parent, sizeof(parent), 7);
  for (unsigned iteration = 0; iteration != 100; ++iteration) {
    uintptr_t cursor = log_cursor();
    strong_frame(parent, 7, 70, 0);
    assert(log_cursor() == cursor);
    assert(ptsan_get_size(escaped_ids[1]) == 0);
    check(parent, sizeof(parent), 7);
    weak_frame(parent, 7, 70, 0);
    assert(log_cursor() == cursor);
    assert(ptsan_get_size(escaped_ids[1]) == 0);
    check(parent, sizeof(parent), 7);
  }
  // Jump out of both nesting orders; the destination's local stays live.
  for (volatile unsigned start = 0; start != 2; ++start) {
    uintptr_t cursor = log_cursor();
    uint64_t next = ptsan_get_stack_cursor();
    int result = setjmp(destination);
    if (!result) {
      if (start)
        strong_frame(parent, 7, 70, 1);
      else
        weak_frame(parent, 7, 70, 1);
      assert(0);
    }
    assert(result == 42);
    assert(log_cursor() == cursor);
    assert(ptsan_get_stack_cursor() == next);
    assert(ptsan_get_size(escaped_ids[0]) == 0);
    assert(ptsan_get_size(escaped_ids[1]) == 0);
    check(parent, sizeof(parent), 7);
  }
  puts("mixed-stack-temporal-ok");
  return 0;
}
#endif
