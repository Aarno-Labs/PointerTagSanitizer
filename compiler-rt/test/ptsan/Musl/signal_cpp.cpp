//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsanxx_musl_strict %s -o %t
// RUN: %run %t
#include "Inputs/signal_test.h"
#include <ucontext.h>

struct Handlers {
  static void info(int signo, siginfo_t *info, void *context) {
    SIGNAL_CHECK(info->si_signo == signo);
    signal_check_live(info, sizeof(*info));
    signal_check_live(context, sizeof(ucontext_t));
  }
};

int main() {
  struct sigaction action = {}, old;
  auto plain = +[](int signo) {
    SIGNAL_CHECK(signo == SIGUSR1);
  };
  action.sa_handler = plain;
  SIGNAL_CHECK(sigaction(SIGUSR1, &action, nullptr) == 0);
  SIGNAL_CHECK(raise(SIGUSR1) == 0);
  action.sa_sigaction = Handlers::info;
  action.sa_flags = SA_SIGINFO;
  SIGNAL_CHECK(sigaction(SIGUSR1, &action, &old) == 0);
  SIGNAL_CHECK(old.sa_handler == plain);
  SIGNAL_CHECK(raise(SIGUSR1) == 0);
  SIGNAL_CHECK(sigaction(SIGUSR1, nullptr, &old) == 0);
  SIGNAL_CHECK(old.sa_sigaction == Handlers::info);
}
