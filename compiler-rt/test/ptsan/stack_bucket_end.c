//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Seed 60298 selects the final bucket. Exhaust it using live stack objects,
// then verify slow fallback and restoration without modifying runtime state.
// RUN: %clang_ptsan -O2 -mllvm -ptsan-random-seed=60298 -mllvm -ptsan-id-bucket-size=64 -mllvm -ptsan-stack-id-thread-buckets=1 -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-instrument-globals=0 %s -o %t
// RUN: %run %t
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-random-seed=60298 -mllvm -ptsan-id-bucket-size=65536 -mllvm -ptsan-stack-id-thread-buckets=1 -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-instrument-globals=0 %s -o %t.id32
// RUN: %run %t.id32
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-random-seed=60298 -mllvm -ptsan-id-bucket-size=65536 -mllvm -ptsan-stack-id-thread-buckets=1 -mllvm -ptsan-stack-temporal-safety=1 -mllvm -ptsan-instrument-globals=0 %s -o %t.strong32
// RUN: %run %t.strong32

#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#ifdef TEST_SIGNALS
#include <signal.h>
#endif

static jmp_buf *Jump;
static uint64_t Limit;
static PTSanID RetiredID;
static int SawLastID;

__attribute__((noinline)) static void touch(char *p) {
  assert(*p == 42);
  assert(get_id(p) != 0);
  if ((uint64_t)get_id(p) == Limit - 1)
    SawLastID = 1;
  __asm__ volatile("" : : "r"(p) : "memory");
}

__attribute__((noinline)) static void child(int jump) {
  char local = 42;
  touch(&local);
  assert(ptsan_get_stack_cursor() == Limit);
  RetiredID = get_id(&local);
  if (jump)
    longjmp(*Jump, 1);
  touch(&local);
}

#ifdef TEST_SIGNALS
static volatile sig_atomic_t Handled;
static void handler(int signo) {
  char local = 42;
  assert(signo == SIGUSR1);
  touch(&local);
  assert(ptsan_get_stack_cursor() == Limit);
  Handled = 1;
}

__attribute__((noinline)) static void install_handler(void) {
  assert(signal(SIGUSR1, handler) != SIG_ERR);
}
#endif

__attribute__((noinline)) static void exhausted(void) {
  assert(ptsan_get_stack_cursor() == Limit);
  assert(SawLastID);
  child(0);
  assert(ptsan_get_stack_cursor() == Limit);
  assert(ptsan_get_size(RetiredID) == 0);
  if (setjmp(*Jump) == 0)
    child(1);
  assert(ptsan_get_stack_cursor() == Limit);
  assert(ptsan_get_size(RetiredID) == 0);
#ifdef TEST_SIGNALS
  assert(raise(SIGUSR1) == 0);
  assert(Handled);
  assert(ptsan_get_stack_cursor() == Limit);
#endif
}

__attribute__((noinline)) static void fill_one(void) {
  char local = 42;
  touch(&local);
  if (ptsan_get_stack_cursor() < Limit)
    fill_one();
  else
    exhausted();
  touch(&local);
}

#ifdef PTSAN_TEST_32_BIT_IDS
// Fill the large ID32 bucket without requiring 65536 recursive C frames.
#define EIGHT(M, P) M(P##0) M(P##1) M(P##2) M(P##3) M(P##4) M(P##5) M(P##6) M(P##7)
#define SIXTY_FOUR(M) EIGHT(M, a) EIGHT(M, b) EIGHT(M, c) EIGHT(M, d) EIGHT(M, e) EIGHT(M, f) EIGHT(M, g) EIGHT(M, h)
#define DECLARE(N) char N = 42;
#define TOUCH(N) touch(&N);

__attribute__((noinline)) static void fill_many(void) {
  SIXTY_FOUR(DECLARE)
  SIXTY_FOUR(TOUCH)
  uint64_t remaining = Limit - ptsan_get_stack_cursor();
  if (remaining >= 64)
    fill_many();
  else if (remaining)
    fill_one();
  else
    exhausted();
  SIXTY_FOUR(TOUCH)
}
#endif

__attribute__((disable_sanitizer_instrumentation)) int main(void) {
  Limit = ptsan_get_stack_limit();
  assert(Limit == (ptsan_id_bits() == 32 ? UINT64_C(1) << 32 : UINT16_MAX));
#ifdef TEST_SIGNALS
  install_handler();
#endif
  Jump = malloc(sizeof(*Jump));
  assert(Jump);
  uint64_t start = ptsan_get_stack_cursor();
  assert(start < Limit);
#ifdef PTSAN_TEST_32_BIT_IDS
  fill_many();
#else
  fill_one();
#endif
  assert(ptsan_get_stack_cursor() == start);
  assert(ptsan_get_stack_limit() == Limit);
  free(Jump);
  return 0;
}
