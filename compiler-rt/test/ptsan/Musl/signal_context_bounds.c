//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict %s -o %t
// RUN: %run %t
// RUN: %ptsan_musl_id32_strict %s -o %t.id32
// RUN: %run %t.id32
// RUN: %ptsan_musl_id32_nonstrict %s -o %t.marked
// RUN: %run %t.marked
//
// Require two distinct, exact-bounds aliases, usable direct fields, preserved
// sigqueue pointer cookies, and retirement on normal return. Changing the
// saved mask verifies that the handler still edits the frame used by sigreturn.
#define _GNU_SOURCE
#include "Inputs/signal_test.h"
#include <errno.h>
#include <ucontext.h>

static char cookie[37];
static uintptr_t expired_info, expired_context;
static volatile sig_atomic_t calls;

static void handler(int signo, siginfo_t *info, void *context) {
  SIGNAL_CHECK(errno == EDOM);
  SIGNAL_CHECK(signo == SIGUSR1);
  signal_check_live(info, sizeof(*info));
  signal_check_live(context, sizeof(ucontext_t));
  SIGNAL_CHECK(get_id(info) != get_id(context));
  SIGNAL_CHECK(info->si_signo == signo && info->si_code == SI_QUEUE);
  SIGNAL_CHECK(info->si_pid == getpid());
  SIGNAL_CHECK(info->si_value.sival_ptr == cookie);
  signal_check_bytes(info->si_value.sival_ptr, sizeof(cookie));
  ucontext_t *uc = context;
#if defined(__x86_64__)
  SIGNAL_CHECK(uc->uc_mcontext.gregs[REG_RIP] != 0);
#elif defined(__aarch64__)
  SIGNAL_CHECK(uc->uc_mcontext.pc != 0);
#endif
  SIGNAL_CHECK(sigismember(&uc->uc_sigmask, SIGUSR2) == 0);
  SIGNAL_CHECK(sigaddset(&uc->uc_sigmask, SIGUSR2) == 0);
  expired_info = (uintptr_t)info;
  expired_context = (uintptr_t)context;
  ++calls;
}

int main(void) {
  struct sigaction action = {0};
  action.sa_sigaction = handler;
  action.sa_flags = SA_SIGINFO;
  SIGNAL_CHECK(sigemptyset(&action.sa_mask) == 0);
  SIGNAL_CHECK(sigaction(SIGUSR1, &action, 0) == 0);
  sigset_t original, unblocked, after;
  SIGNAL_CHECK(sigemptyset(&unblocked) == 0);
  SIGNAL_CHECK(sigaddset(&unblocked, SIGUSR1) == 0);
  SIGNAL_CHECK(sigaddset(&unblocked, SIGUSR2) == 0);
  SIGNAL_CHECK(sigprocmask(SIG_UNBLOCK, &unblocked, &original) == 0);
  signal_touch(cookie, sizeof(cookie));
  union sigval value;
  value.sival_ptr = cookie;
  errno = EDOM;
  SIGNAL_CHECK(sigqueue(getpid(), SIGUSR1, value) == 0);
  SIGNAL_CHECK(errno == EDOM && calls == 1);
  signal_check_retired(expired_info);
  signal_check_retired(expired_context);
  SIGNAL_CHECK(sigprocmask(SIG_SETMASK, 0, &after) == 0);
  SIGNAL_CHECK(sigismember(&after, SIGUSR2) == 1);
  SIGNAL_CHECK(sigprocmask(SIG_SETMASK, &original, 0) == 0);
  return 0;
}
