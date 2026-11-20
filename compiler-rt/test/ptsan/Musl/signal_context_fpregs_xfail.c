//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// The kernel supplies uc_mcontext.fpregs as an untagged pointer to floating-
// point state in the signal frame. PTSan tags the enclosing ucontext_t, but
// does not yet give this embedded pointer its own bounds.
//
// REQUIRES: x86_64-target-arch
// XFAIL: *
// RUN: %ptsan_musl_strict %s -o %t
// RUN: %run %t

#define _GNU_SOURCE
#include <signal.h>
#include <ucontext.h>

static volatile unsigned short observed;

static void handler(int signal, siginfo_t *info, void *context) {
  (void)signal;
  (void)info;
  ucontext_t *uc = context;
  observed = uc->uc_mcontext.fpregs->cwd;
}

int main(void) {
  struct sigaction action = {0};
  action.sa_sigaction = handler;
  action.sa_flags = SA_SIGINFO;
  if (sigemptyset(&action.sa_mask) || sigaction(SIGUSR1, &action, 0) ||
      raise(SIGUSR1)) {
    return 1;
  }
  return 0;
}
