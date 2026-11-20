//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -ffunction-sections -fdata-sections -c %S/Inputs/signal_external.c -o %t.o
// RUN: %ptsan_musl_strict -ffunction-sections -fdata-sections %s %t.o -Wl,--gc-sections -o %t
// RUN: %run %t
#include "Inputs/signal_test.h"

void external_handler(int);
void external_info_handler(int, siginfo_t *, void *);
void external_alias(int);

void overridden_handler(int signo) { external_handler(signo); }

int main(void) {
  struct sigaction action = {0}, old;
  action.sa_handler = external_handler;
  SIGNAL_CHECK(sigaction(SIGUSR1, &action, 0) == 0);
  SIGNAL_CHECK(raise(SIGUSR1) == 0);
  action.sa_sigaction = external_info_handler;
  action.sa_flags = SA_SIGINFO;
  SIGNAL_CHECK(sigaction(SIGUSR1, &action, &old) == 0);
  SIGNAL_CHECK(old.sa_handler == external_handler);
  SIGNAL_CHECK(raise(SIGUSR1) == 0);
  SIGNAL_CHECK(sigaction(SIGUSR1, 0, &old) == 0);
  SIGNAL_CHECK(old.sa_sigaction == external_info_handler);
  action.sa_flags = 0;
  action.sa_handler = external_alias;
  SIGNAL_CHECK(sigaction(SIGUSR1, &action, 0) == 0);
  SIGNAL_CHECK(raise(SIGUSR1) == 0);
  SIGNAL_CHECK(sigaction(SIGUSR1, 0, &old) == 0);
  SIGNAL_CHECK(old.sa_handler == external_alias);
  action.sa_handler = overridden_handler;
  SIGNAL_CHECK(sigaction(SIGUSR1, &action, 0) == 0);
  SIGNAL_CHECK(raise(SIGUSR1) == 0);
  SIGNAL_CHECK(sigaction(SIGUSR1, 0, &old) == 0);
  SIGNAL_CHECK(old.sa_handler == overridden_handler);
  return 0;
}
