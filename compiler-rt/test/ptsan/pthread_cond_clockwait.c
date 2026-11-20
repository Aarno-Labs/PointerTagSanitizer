//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan -O2 -pthread -S -emit-llvm -o %t.ll %s
// RUN: FileCheck %s --check-prefix=IR < %t.ll
// RUN: %clang_ptsan -O2 -pthread -mllvm -ptsan-abort-on-error -o %t %s
// RUN: %t 2>&1 | FileCheck %s

// Abseil's pthread waiter uses this entry point for monotonic-clock deadlines.
// All three pointer arguments come from a tagged allocation. An expired
// deadline makes the wait deterministic and must return ETIMEDOUT, not fail
// inside libc or pass a tagged timeout pointer to the kernel.

// IR: call i32 @pthread_cond_clockwait(
// CHECK-NOT: Found bad
// CHECK: pthread-cond-clockwait-ok
// CHECK-NOT: Found bad

#define _GNU_SOURCE
#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

int main(void) {
  struct {
    pthread_cond_t cond;
    pthread_mutex_t mutex;
    struct timespec deadline;
  } *state = malloc(sizeof(*state));
  assert(state && get_id(state) != 0);
  assert(pthread_cond_init(&state->cond, NULL) == 0);
  assert(pthread_mutex_init(&state->mutex, NULL) == 0);
  state->deadline.tv_sec = 0;
  state->deadline.tv_nsec = 0;

  assert(pthread_mutex_lock(&state->mutex) == 0);
  assert(pthread_cond_clockwait(&state->cond, &state->mutex, CLOCK_MONOTONIC,
                                &state->deadline) == ETIMEDOUT);
  assert(pthread_mutex_unlock(&state->mutex) == 0);

  assert(pthread_cond_destroy(&state->cond) == 0);
  assert(pthread_mutex_destroy(&state->mutex) == 0);
  free(state);
  puts("pthread-cond-clockwait-ok");
  return 0;
}
