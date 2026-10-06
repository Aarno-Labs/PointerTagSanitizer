//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise musl signal and POSIX timer APIs whose syscall arguments are
// fixed local structures or direct public signal objects.
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t good
// RUN: not %run %t bad-info 2> %t.info
// RUN: FileCheck %s < %t.info
// RUN: not %run %t bad-mask 2> %t.mask
// RUN: FileCheck %s < %t.mask
//
// CHECK: Found bad

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t handled_signal;

static void handle_signal(int signal) { handled_signal = signal; }

static int test_good(void) {
  void *cookie = malloc(1);
  if (!cookie) {
    return 1;
  }

  struct sigaction action;
  struct sigaction old_action;
  memset(&action, 0, sizeof(struct sigaction));
  action.sa_handler = handle_signal;
  if (sigemptyset(&action.sa_mask) != 0 ||
      sigaction(SIGUSR2, &action, &old_action) != 0) {
    free(cookie);
    return 2;
  }

  if (raise(SIGUSR2) != 0 || handled_signal != SIGUSR2) {
    (void)sigaction(SIGUSR2, &old_action, NULL);
    free(cookie);
    return 3;
  }
  if (sigaction(SIGUSR2, &old_action, NULL) != 0) {
    free(cookie);
    return 4;
  }

  sigset_t signal_set;
  sigset_t old_mask;
  if (sigemptyset(&signal_set) != 0 || sigaddset(&signal_set, SIGUSR1) != 0 ||
      sigprocmask(SIG_BLOCK, &signal_set, &old_mask) != 0) {
    free(cookie);
    return 5;
  }

  union sigval value;
  value.sival_ptr = cookie;
  if (sigqueue(getpid(), SIGUSR1, value) != 0) {
    (void)sigprocmask(SIG_SETMASK, &old_mask, NULL);
    free(cookie);
    return 6;
  }

  siginfo_t info;
  memset(&info, 0, sizeof(siginfo_t));
  struct timespec timeout = {1, 0};
  int received = sigtimedwait(&signal_set, &info, &timeout);
  int mask_result = sigprocmask(SIG_SETMASK, &old_mask, NULL);
  if (received != SIGUSR1 || info.si_code != SI_QUEUE ||
      info.si_value.sival_ptr != cookie || mask_result != 0) {
    free(cookie);
    return 7;
  }

  struct sigevent event;
  memset(&event, 0, sizeof(struct sigevent));
  event.sigev_notify = SIGEV_NONE;
  event.sigev_value.sival_ptr = cookie;
  timer_t timer;
  if (timer_create(CLOCK_MONOTONIC, &event, &timer) != 0) {
    free(cookie);
    return 8;
  }
  int timer_result = timer_delete(timer);
  free(cookie);
  return timer_result == 0 ? 0 : 9;
}

static int test_bad_info(void) {
  sigset_t signal_set;
  if (sigemptyset(&signal_set) != 0 || sigaddset(&signal_set, SIGUSR1) != 0 ||
      sigprocmask(SIG_BLOCK, &signal_set, NULL) != 0) {
    return 1;
  }

  union sigval value;
  value.sival_int = 1;
  if (sigqueue(getpid(), SIGUSR1, value) != 0) {
    return 2;
  }

  void *info = calloc(1, sizeof(siginfo_t) - 1);
  if (!info) {
    return 3;
  }
  struct timespec timeout = {1, 0};
  (void)sigtimedwait(&signal_set, (siginfo_t *)info, &timeout);
  free(info);
  return 0;
}

static int test_bad_mask(void) {
  void *signal_set = calloc(1, sizeof(sigset_t) - 1);
  if (!signal_set) {
    return 1;
  }
  struct timespec timeout = {0, 0};
  (void)sigtimedwait((const sigset_t *)signal_set, NULL, &timeout);
  free(signal_set);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }
  if (!strcmp(argv[1], "good")) {
    return test_good();
  }
  if (!strcmp(argv[1], "bad-info")) {
    return test_bad_info();
  }
  if (!strcmp(argv[1], "bad-mask")) {
    return test_bad_mask();
  }
  return 2;
}
