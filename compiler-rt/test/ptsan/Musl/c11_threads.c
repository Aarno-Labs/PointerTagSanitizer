// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -pthread -o %t %s
// RUN: %run %t

#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <threads.h>

static _Thread_local int value = 7;

static void check_worker(int signals_blocked) {
  sigset_t mask;
  assert(pthread_sigmask(SIG_SETMASK, 0, &mask) == 0);
  assert(sigismember(&mask, SIGUSR1) == signals_blocked);
  assert(value == 7);
  value = 42;
  char buffer[32];
  assert(snprintf(buffer, sizeof(buffer), "%s %d", "thread", value) == 9);
  assert(strcmp(buffer, "thread 42") == 0);
}

static int c11_worker(void *arg) {
  check_worker(1);
  return *(int *)arg;
}

static int c11_exit_worker(void *arg) {
  check_worker(1);
  thrd_exit(*(int *)arg);
}

static void *pthread_worker(void *arg) {
  check_worker(0);
  return arg;
}

int main(void) {
  sigset_t mask;
  assert(sigemptyset(&mask) == 0);
  assert(sigaddset(&mask, SIGUSR1) == 0);
  assert(pthread_sigmask(SIG_UNBLOCK, &mask, 0) == 0);

  // Exercise the internal __pthread_create path and C11's integer results.
  int values[] = {0, 42, -42, INT_MIN, INT_MAX};
  for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
    thrd_t thread;
    int result;
    assert(thrd_create(&thread, c11_worker, &values[i]) == thrd_success);
    assert(thrd_join(thread, &result) == thrd_success);
    assert(result == values[i]);
  }
  thrd_t thread;
  int result;
  assert(thrd_create(&thread, c11_exit_worker, &values[2]) == thrd_success);
  assert(thrd_join(thread, &result) == thrd_success);
  assert(result == values[2]);

  // The public alias still initializes pthreads and preserves pointer results
  // and the creator's signal mask. Creating C11 threads must not alter it.
  pthread_t pthread;
  void *pointer_result;
  assert(pthread_create(&pthread, 0, pthread_worker, &values[1]) == 0);
  assert(pthread_join(pthread, &pointer_result) == 0);
  assert(pointer_result == &values[1]);
  assert(value == 7);
  assert(pthread_sigmask(SIG_SETMASK, 0, &mask) == 0);
  assert(sigismember(&mask, SIGUSR1) == 0);
  return 0;
}
