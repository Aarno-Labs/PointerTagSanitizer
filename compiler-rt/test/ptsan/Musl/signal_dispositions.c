//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict %s -o %t
// RUN: %run %t
//
// Compatibility controls for signal registration and delivery:
// original handler identities, oldact, failed registration, SIG_DFL/SIG_IGN,
// signal(), SA_RESETHAND, sa_mask, and default same-signal deferral.
#define _GNU_SOURCE
#include "Inputs/signal_test.h"
#include <errno.h>

static volatile sig_atomic_t calls, active, reset_seen;

static void plain_handler(int signo) {
  SIGNAL_CHECK(signo == SIGUSR1 && active == 0);
  active = 1;
  sigset_t mask;
  SIGNAL_CHECK(sigprocmask(SIG_SETMASK, 0, &mask) == 0);
  SIGNAL_CHECK(sigismember(&mask, SIGUSR1) == 1);
  SIGNAL_CHECK(sigismember(&mask, SIGUSR2) == 1);
  if (++calls == 1) {
    SIGNAL_CHECK(raise(SIGUSR1) == 0);
    SIGNAL_CHECK(calls == 1); // Pending until this invocation returns.
  }
  active = 0;
}

static void info_handler(int signo, siginfo_t *info, void *context) {
  (void)info;
  (void)context; // Context access is checked in separate bounds tests.
  SIGNAL_CHECK(signo == SIGUSR1);
  ++calls;
}

static void no_args_handler(void) { ++calls; }

static void reset_handler(int signo) {
  struct sigaction current;
  SIGNAL_CHECK(sigaction(signo, 0, &current) == 0);
  SIGNAL_CHECK(current.sa_handler == SIG_DFL);
  reset_seen = 1;
}

int main(void) {
  sigset_t empty, original, after;
  SIGNAL_CHECK(sigemptyset(&empty) == 0);
  SIGNAL_CHECK(sigprocmask(SIG_SETMASK, &empty, &original) == 0);
  struct sigaction action = {0}, previous, current;
  action.sa_handler = plain_handler;
  action.sa_flags = SA_RESTART;
  SIGNAL_CHECK(sigemptyset(&action.sa_mask) == 0);
  SIGNAL_CHECK(sigaddset(&action.sa_mask, SIGUSR2) == 0);
  SIGNAL_CHECK(sigaction(SIGUSR1, &action, &previous) == 0);
  SIGNAL_CHECK(sigaction(SIGUSR1, 0, &current) == 0);
  SIGNAL_CHECK(current.sa_handler == plain_handler);
  SIGNAL_CHECK((current.sa_flags & SA_RESTART) != 0);
  SIGNAL_CHECK(sigismember(&current.sa_mask, SIGUSR2) == 1);
  errno = EDOM;
  SIGNAL_CHECK(raise(SIGUSR1) == 0);
  SIGNAL_CHECK(calls == 2 && errno == EDOM);
  SIGNAL_CHECK(sigprocmask(SIG_SETMASK, 0, &after) == 0);
  SIGNAL_CHECK(sigismember(&after, SIGUSR1) == 0);
  SIGNAL_CHECK(sigismember(&after, SIGUSR2) == 0);

  action.sa_sigaction = info_handler;
  action.sa_flags = SA_SIGINFO;
  SIGNAL_CHECK(sigaction(SIGUSR1, &action, &previous) == 0);
  SIGNAL_CHECK(previous.sa_handler == plain_handler);
  SIGNAL_CHECK((previous.sa_flags & SA_SIGINFO) == 0);
  SIGNAL_CHECK(raise(SIGUSR1) == 0 && calls == 3);
  SIGNAL_CHECK(sigaction(0, &action, 0) == -1 && errno == EINVAL);
  SIGNAL_CHECK(sigaction(SIGKILL, &action, 0) == -1 && errno == EINVAL);
  SIGNAL_CHECK(sigaction(SIGUSR1, 0, &current) == 0);
  SIGNAL_CHECK(current.sa_sigaction == info_handler);
  SIGNAL_CHECK((current.sa_flags & SA_SIGINFO) != 0);

  action.sa_handler = SIG_IGN;
  action.sa_flags = 0;
  SIGNAL_CHECK(sigaction(SIGUSR1, &action, &previous) == 0);
  SIGNAL_CHECK(previous.sa_sigaction == info_handler);
  SIGNAL_CHECK((previous.sa_flags & SA_SIGINFO) != 0);
  SIGNAL_CHECK(raise(SIGUSR1) == 0 && calls == 3);
  SIGNAL_CHECK(signal(SIGUSR1, reset_handler) == SIG_IGN);
  SIGNAL_CHECK(signal(SIGUSR1, SIG_IGN) == reset_handler);
  action.sa_handler = reset_handler;
  action.sa_flags = SA_RESETHAND;
  SIGNAL_CHECK(sigaction(SIGUSR1, &action, 0) == 0);
  SIGNAL_CHECK(raise(SIGUSR1) == 0 && reset_seen == 1);
  SIGNAL_CHECK(sigaction(SIGUSR1, 0, &current) == 0);
  SIGNAL_CHECK(current.sa_handler == SIG_DFL);
  // Legacy zero-argument handlers retain their identity through both APIs.
  void (*no_args)(int) = (void (*)(int))no_args_handler;
  SIGNAL_CHECK(signal(SIGUSR1, no_args) == SIG_DFL);
  SIGNAL_CHECK(sigaction(SIGUSR1, 0, &current) == 0);
  SIGNAL_CHECK(current.sa_handler == no_args);
  SIGNAL_CHECK(raise(SIGUSR1) == 0 && calls == 4);
  action.sa_handler = SIG_IGN;
  action.sa_flags = 0;
  SIGNAL_CHECK(sigaction(SIGUSR1, &action, &previous) == 0);
  SIGNAL_CHECK(previous.sa_handler == no_args);
  action.sa_handler = no_args;
  SIGNAL_CHECK(sigaction(SIGUSR1, &action, 0) == 0);
  SIGNAL_CHECK(raise(SIGUSR1) == 0 && calls == 5);
  SIGNAL_CHECK(signal(SIGUSR1, SIG_IGN) == no_args);
  SIGNAL_CHECK(sigprocmask(SIG_SETMASK, &original, 0) == 0);
  return 0;
}
