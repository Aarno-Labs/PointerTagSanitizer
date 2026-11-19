//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
#include "signal_test.h"
#include <ucontext.h>

// No address-taking in this translation unit. An external definition must
// still receive an entry for callers compiled separately.
void external_handler(int signo) {
  char bytes[37];
  SIGNAL_CHECK(signo == SIGUSR1);
  signal_touch(bytes, sizeof(bytes));
  signal_check_live(bytes, sizeof(bytes));
}

void external_alias(int) __attribute__((alias("external_handler")));

__attribute__((weak)) void overridden_handler(int signo) {
  (void)signo;
  _exit(2);
}

void external_info_handler(int signo, siginfo_t *info, void *context) {
  external_handler(signo);
  SIGNAL_CHECK(info->si_signo == signo);
  signal_check_live(info, sizeof(*info));
  signal_check_live(context, sizeof(ucontext_t));
}
