//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan -O2 -pthread -mllvm -ptsan-stack-temporal-safety=0 %s -o %t
// RUN: %t 2>&1 | FileCheck %s
// RUN: %clang_ptsan -O2 -pthread -DTEST_TEMPORAL=1 %s -o %t.temporal
// RUN: %t.temporal 2>&1 | FileCheck %s
// RUN: %clang_ptsan -O2 -pthread -DTEST_TEMPORAL=1 -mllvm -ptsan-no-stack-id-fast-path %s -o %t.slow
// RUN: %t.slow 2>&1 | FileCheck %s
// RUN: %clang_ptsan -O2 -pthread -DTEST_TEMPORAL=1 -mllvm -ptsan-stack-ids-per-bucket=1 %s -o %t.small
// RUN: %t.small 2>&1 | FileCheck %s
// CHECK-NOT: Found bad
// CHECK: setjmp-cleanup-ok
// CHECK-NOT: Found bad
// CHECK: Unreclaimed IDs at exit: 0

#define _GNU_SOURCE
#include <alloca.h>
#include <assert.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern __thread uint16_t ptsan_next_stack_id;
extern struct Bounds {
  uint64_t base, span;
} ptsan_bounds[65536];

// Read runtime metadata without requesting an application TLS ID for it.
__attribute__((disable_sanitizer_instrumentation, noinline))
static uint16_t stack_cursor(void) {
  return ptsan_next_stack_id;
}

__attribute__((disable_sanitizer_instrumentation, noinline))
static int retired(uintptr_t pointer) {
  unsigned id = pointer >> 48;
  return id != 0 && ptsan_bounds[id].base == 0 && ptsan_bounds[id].span == 0;
}

static volatile unsigned count = 5;
static volatile unsigned size = 37;
static unsigned iteration;
static char *retained;
static char *heap;
static uintptr_t expired_fixed, expired_dynamic;

__attribute__((noinline)) static void fill(char *p, unsigned n) {
  p[0] = 17;
  p[n - 1] = 29;
  __asm__ volatile("" : : "r"(p) : "memory");
}

__attribute__((noinline)) static void check(const char *p, unsigned n) {
  assert(p[0] == 17);
  assert(p[n - 1] == 29);
}

__attribute__((noinline)) static void repeated(jmp_buf env, int jump) {
  for (unsigned i = 0; i < count; ++i) {
    unsigned n = size;
    char *p = (char *)alloca(n);
    fill(p, n);
    if (jump && i + 1 == count) {
      expired_dynamic = (uintptr_t)p;
      // Heap objects allocated in an abandoned frame must remain live.
      heap = (char *)malloc(n);
      fill(heap, n);
      // Indirect longjmp still lands at the instrumented setjmp continuation.
      void (*volatile perform_jump)(jmp_buf, int) = longjmp;
      perform_jump(env, 42);
    }
    check(p, n);
  }
}

// Exercise each small fixed-ID batch and the generic (>4 IDs) batch. These
// also return normally, so tracking must balance both kinds of exit.
#define FRAME(N, NEXT)                                                         \
  __attribute__((noinline)) static void frame##N(jmp_buf env, int jump) {        \
    char a[32], b[32], c[32], d[32], e[32];                                     \
    fill(a, 32);                                                              \
    if (N > 1) { fill(b, 32); }                                                \
    if (N > 2) { fill(c, 32); }                                                \
    if (N > 3) { fill(d, 32); }                                                \
    if (N > 4) { fill(e, 32); }                                                \
    if (N == 1) { expired_fixed = (uintptr_t)a; }                               \
    NEXT(env, jump);                                                           \
    check(a, 32);                                                             \
    if (N > 1) { check(b, 32); }                                               \
    if (N > 2) { check(c, 32); }                                               \
    if (N > 3) { check(d, 32); }                                               \
    if (N > 4) { check(e, 32); }                                               \
  }

FRAME(5, repeated)
FRAME(4, frame5)
FRAME(3, frame4)
FRAME(2, frame3)
FRAME(1, frame2)

__attribute__((noinline)) static void exercise(unsigned mode) {
  sigjmp_buf env;
  char fixed[32];
  fill(fixed, sizeof(fixed));
  uint16_t before = stack_cursor();
  iteration = 0;
  for (;;) {
    // This frame's container survives each jump, including IDs added since
    // the entry checkpoint. The buffer itself predates setjmp's saved SP.
    unsigned n = size;
    retained = (char *)alloca(n);
    fill(retained, n);
    int value;
    if (mode == 0) {
      value = setjmp(env);
    } else if (mode == 1) {
      value = _setjmp(env);
    } else {
      value = sigsetjmp(env, mode == 3);
    }
    if (value == 0) {
      frame1(env, 0);
      frame1(env, 1);
      assert(0);
    }
    assert(value == 42);
    assert(stack_cursor() == before);
    assert(retired(expired_dynamic));
#ifdef TEST_TEMPORAL
    assert(retired(expired_fixed));
#endif
    check(fixed, sizeof(fixed));
    check(retained, size);
    check(heap, size);
    free(heap);
    if (++iteration == 100) {
      break;
    }
  }
}

static void *worker(void *unused) {
  for (unsigned mode = 0; mode != 4; ++mode) {
    exercise(mode);
  }
  return 0;
}

int main(void) {
  worker(0);
  pthread_t thread;
  assert(pthread_create(&thread, 0, worker, 0) == 0);
  assert(pthread_join(thread, 0) == 0);
  fprintf(stderr, "setjmp-cleanup-ok\n");
}
