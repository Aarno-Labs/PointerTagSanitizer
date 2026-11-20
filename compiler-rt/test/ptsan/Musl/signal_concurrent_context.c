//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -pthread %s -o %t
// RUN: %run %t
//
// Two threads hold their signal frames live simultaneously. Check uniqueness
// across threads, then retirement after joining both threads. Lock-free atomic
// rendezvous keep pthread synchronization routines out of the handlers.
#define _GNU_SOURCE
#include "Inputs/signal_test.h"
#include <pthread.h>
#include <ucontext.h>

_Static_assert(__atomic_always_lock_free(sizeof(unsigned), 0),
               "signal atomics");
static unsigned entered, checked;
static _Thread_local unsigned index;
static uintptr_t objects[2][3];

static void handler(int signo, siginfo_t *info, void *context) {
  char local[37];
  unsigned self = index;
  SIGNAL_CHECK(signo == SIGUSR1 && self < 2);
  signal_touch(local, sizeof(local));
  signal_check_live(info, sizeof(*info));
  signal_check_live(context, sizeof(ucontext_t));
  objects[self][0] = (uintptr_t)info;
  objects[self][1] = (uintptr_t)context;
  objects[self][2] = (uintptr_t)local;
  __atomic_fetch_add(&entered, 1, __ATOMIC_ACQ_REL);
  while (__atomic_load_n(&entered, __ATOMIC_ACQUIRE) != 2) {
  }
  for (unsigned i = 0; i != 3; ++i) {
    for (unsigned j = 0; j != 3; ++j) {
      SIGNAL_CHECK(get_id(objects[0][i]) != get_id(objects[1][j]));
    }
  }
  signal_check_live((void *)objects[1 - self][0], sizeof(siginfo_t));
  signal_check_live((void *)objects[1 - self][1], sizeof(ucontext_t));
  SIGNAL_CHECK(info->si_signo == signo);
  __atomic_fetch_add(&checked, 1, __ATOMIC_ACQ_REL);
  while (__atomic_load_n(&checked, __ATOMIC_ACQUIRE) != 2) {
  }
}

static void *worker(void *argument) {
  index = (uintptr_t)argument;
  uint64_t limit = signal_stack_limit();
  SIGNAL_CHECK(raise(SIGUSR1) == 0);
  SIGNAL_CHECK(signal_stack_limit() == limit);
  return 0;
}

int main(void) {
  struct sigaction action = {0};
  action.sa_sigaction = handler;
  action.sa_flags = SA_SIGINFO;
  SIGNAL_CHECK(sigemptyset(&action.sa_mask) == 0);
  SIGNAL_CHECK(sigaction(SIGUSR1, &action, 0) == 0);
  uint64_t baseline = signal_live_stack_ids();
  pthread_t threads[2];
  for (unsigned i = 0; i != 2; ++i) {
    SIGNAL_CHECK(pthread_create(&threads[i], 0, worker, (void *)(uintptr_t)i) ==
                 0);
  }
  for (unsigned i = 0; i != 2; ++i) {
    SIGNAL_CHECK(pthread_join(threads[i], 0) == 0);
  }
  SIGNAL_CHECK(signal_live_stack_ids() == baseline);
  for (unsigned i = 0; i != 2; ++i) {
    for (unsigned j = 0; j != 3; ++j) {
      signal_check_retired(objects[i][j]);
    }
  }
  return 0;
}
