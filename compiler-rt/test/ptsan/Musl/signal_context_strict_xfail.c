//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// XFAIL: *
// RUN: %ptsan_musl_strict %s -o %t
// RUN: %run %t
// RUN: %run %t info

// The kernel supplies untagged siginfo_t/ucontext_t pointers to SA_SIGINFO
// handlers. These valid reads should succeed, but strict mode rejects ID zero.
// Musl's cross-thread cancellation handler also reads the supplied context.
//
// A signal-entry wrapper cannot simply allocate temporary IDs: a signal can
// interrupt the ID allocator while it holds its lock, causing self-deadlock,
// or interrupt an update to the thread's stack-ID tracking vector. Handler
// instrumentation can enter those paths too, even if the wrapper reserves IDs.
// Nested handlers need distinct live IDs, and siglongjmp/pthread_exit can skip
// the wrapper's normal retirement. A fix must handle those lifetimes and
// signal re-entry, plus embedded context pointers consumed by sigreturn.
// Keep this deferred rather than introducing an unsafe allocating wrapper.

#define _GNU_SOURCE
#include <signal.h>
#include <ucontext.h>

static volatile sig_atomic_t observed;
static volatile sig_atomic_t use_info;

static void handler(int signal, siginfo_t *info, void *context) {
  if (use_info) {
    observed = info->si_signo;
  } else {
    ucontext_t *uc = context;
#if defined(__aarch64__)
    observed = uc->uc_mcontext.pc != 0 ? signal : 0;
#elif defined(__x86_64__)
    observed = uc->uc_mcontext.gregs[REG_RIP] != 0 ? signal : 0;
#else
#error "Unsupported architecture for the signal-context fixture"
#endif
  }
}

int main(int argc, char **argv) {
  (void)argv;
  use_info = argc > 1;
  struct sigaction action = {0};
  action.sa_sigaction = handler;
  action.sa_flags = SA_SIGINFO;
  if (sigemptyset(&action.sa_mask) || sigaction(SIGUSR1, &action, 0) ||
      raise(SIGUSR1))
    return 2;
  return observed != SIGUSR1;
}
