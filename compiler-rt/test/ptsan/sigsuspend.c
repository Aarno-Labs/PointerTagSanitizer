//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> err.txt
// RUN: cat err.txt | FileCheck %s

// sigsuspend returns only after a handler runs: arm SIGALRM, block it, then
// sigsuspend() with a mask that unblocks it. After it returns, in_libc must be
// back to 0 and tagging still live.

// CHECK: handler ran
// CHECK: in_libc after sigsuspend: 0
// CHECK: Found bad

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <threads.h>

extern thread_local int ptsan_in_libc;

static volatile sig_atomic_t got_signal = 0;
// Runs while in_libc=1, so opt out of the -ptsan-runtime-validate
// "in_libc == 0" check that instrumented functions carry.
__attribute__((disable_sanitizer_instrumentation))
static void on_alarm(int sig) { (void)sig; got_signal = 1; }

int __attribute__((noinline)) do_access(char *buf, int idx) {
  return buf[idx];
}

int main(void) {
  struct sigaction sa;
  sa.sa_handler = on_alarm;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGALRM, &sa, NULL);

  // Block SIGALRM so it stays pending until sigsuspend unblocks it.
  sigset_t block, oldmask, suspend;
  sigemptyset(&block);
  sigaddset(&block, SIGALRM);
  sigprocmask(SIG_BLOCK, &block, &oldmask);

  alarm(1);

  // suspend mask = the old mask minus SIGALRM, so SIGALRM is delivered during
  // the suspend and the handler returns control to us.
  suspend = oldmask;
  sigdelset(&suspend, SIGALRM);
  sigsuspend(&suspend);   // returns (errno EINTR) after the handler runs

  if (got_signal)
    fprintf(stderr, "handler ran\n");
  fprintf(stderr, "in_libc after sigsuspend: %d\n", ptsan_in_libc);

  // Tagging must still be live: this allocation comes back tagged, so the OOB
  // read below is reported.
  char *buf = (char *)malloc(20);
  for (int i = 0; i < 20; i++) buf[i] = 'A';
  fprintf(stderr, "oob = %c\n", do_access(buf, 23));

  fprintf(stderr, "reached end without a report!\n");
  return 0;
}
