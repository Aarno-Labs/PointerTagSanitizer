//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -pthread %s -o %t
// RUN: %run %t
//
// Bounded stress control for concurrent registration and delivery. Each
// handler requires its own action's mask. An atomic handler-pointer table
// alone is insufficient if it can disagree with the kernel-installed action.
// No assertion requires a particular winner of a concurrent replacement.
#define _GNU_SOURCE
#include "Inputs/signal_test.h"
#include <pthread.h>
#include <sched.h>

static int done;
static unsigned deliveries;

static void check_mask(int expected) {
  sigset_t mask;
  SIGNAL_CHECK(sigprocmask(SIG_SETMASK, 0, &mask) == 0);
  SIGNAL_CHECK(sigismember(&mask, SIGUSR2) == expected);
  __atomic_fetch_add(&deliveries, 1, __ATOMIC_RELAXED);
}

static void handler_a(int signo, siginfo_t *info, void *context) {
  (void)info;
  (void)context;
  SIGNAL_CHECK(signo == SIGUSR1);
  check_mask(1);
}

static void handler_b(int signo, siginfo_t *info, void *context) {
  (void)info;
  (void)context;
  SIGNAL_CHECK(signo == SIGUSR1);
  check_mask(0);
}

static void *replace_actions(void *unused) {
  (void)unused;
  for (unsigned i = 0; i != 4000; ++i) {
    struct sigaction action = {0}, previous;
    action.sa_flags = SA_SIGINFO;
    action.sa_sigaction = i % 2 ? handler_a : handler_b;
    SIGNAL_CHECK(sigemptyset(&action.sa_mask) == 0);
    if (i % 2) {
      SIGNAL_CHECK(sigaddset(&action.sa_mask, SIGUSR2) == 0);
    }
    SIGNAL_CHECK(sigaction(SIGUSR1, &action, &previous) == 0);
    SIGNAL_CHECK(previous.sa_sigaction == (i % 2 ? handler_b : handler_a));
    SIGNAL_CHECK((previous.sa_flags & SA_SIGINFO) != 0);
    SIGNAL_CHECK(sigismember(&previous.sa_mask, SIGUSR2) == !(i % 2));
    // Require deliveries between batches of replacements, so the writer
    // cannot finish before delivery starts. Hitting the replacement itself
    // remains probabilistic.
    if ((i + 1) % 128 == 0) {
      unsigned before = __atomic_load_n(&deliveries, __ATOMIC_RELAXED);
      while (__atomic_load_n(&deliveries, __ATOMIC_RELAXED) == before) {
        sched_yield();
      }
    }
  }
  __atomic_store_n(&done, 1, __ATOMIC_RELEASE);
  return 0;
}

int main(void) {
  sigset_t empty;
  SIGNAL_CHECK(sigemptyset(&empty) == 0);
  SIGNAL_CHECK(sigprocmask(SIG_SETMASK, &empty, 0) == 0);
  struct sigaction action = {0};
  action.sa_sigaction = handler_a;
  action.sa_flags = SA_SIGINFO;
  SIGNAL_CHECK(sigemptyset(&action.sa_mask) == 0);
  SIGNAL_CHECK(sigaddset(&action.sa_mask, SIGUSR2) == 0);
  SIGNAL_CHECK(sigaction(SIGUSR1, &action, 0) == 0);
  SIGNAL_CHECK(raise(SIGUSR1) == 0);
  pthread_t writer;
  SIGNAL_CHECK(pthread_create(&writer, 0, replace_actions, 0) == 0);
  do {
    SIGNAL_CHECK(raise(SIGUSR1) == 0);
  } while (!__atomic_load_n(&done, __ATOMIC_ACQUIRE));
  SIGNAL_CHECK(pthread_join(writer, 0) == 0);
  SIGNAL_CHECK(__atomic_load_n(&deliveries, __ATOMIC_RELAXED) >= 2);
  return 0;
}
