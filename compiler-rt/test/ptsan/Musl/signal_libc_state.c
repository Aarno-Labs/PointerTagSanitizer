//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict %s -o %t
// RUN: %run %t
// Check reset on entry, restoration on normal return, and nonlocal escape.
// Jump destinations also clear libc state when a jump skips boundary cleanup.
#include "Inputs/signal_test.h"
#include <setjmp.h>

extern __thread uint32_t ptsan_in_libc;
static sigjmp_buf target;
static jmp_buf plain_target;
static volatile sig_atomic_t escape;

static SIGNAL_RAW uint32_t libc_state(void) { return ptsan_in_libc; }

static SIGNAL_RAW int interrupt_boundary(void) {
  ptsan_in_libc = 7;
  int result = raise(SIGUSR1);
  int restored = ptsan_in_libc == 7;
  ptsan_in_libc = 0;
  return result == 0 && restored;
}

static SIGNAL_RAW void jump_from_boundary(int use_siglongjmp, void *env) {
  ptsan_in_libc = 7;
  if (use_siglongjmp) {
    siglongjmp(env, 1);
  } else {
    longjmp(env, 1);
  }
}

static void plain(int signo) {
  SIGNAL_CHECK(signo == SIGUSR1 && libc_state() == 0);
  if (escape) {
    siglongjmp(target, 1);
  }
}

static void no_args(void) { plain(SIGUSR1); }

static void info(int signo, siginfo_t *info, void *context) {
  (void)context;
  SIGNAL_CHECK(info->si_signo == signo);
  plain(signo);
}

int main(void) {
  for (unsigned kind = 0; kind != 3; ++kind) {
    struct sigaction action = {0};
    if (kind == 2) {
      action.sa_flags = SA_SIGINFO;
      action.sa_sigaction = info;
    } else if (kind == 1) {
      action.sa_handler = plain;
    } else {
      action.sa_handler = (void (*)(int))no_args;
    }
    SIGNAL_CHECK(sigaction(SIGUSR1, &action, 0) == 0);
    escape = 0;
    SIGNAL_CHECK(interrupt_boundary());
    SIGNAL_CHECK(libc_state() == 0);
    escape = 1;
    if (sigsetjmp(target, 1) == 0) {
      interrupt_boundary();
      SIGNAL_CHECK(0);
    }
    SIGNAL_CHECK(libc_state() == 0);
  }
  if (sigsetjmp(target, 1) == 0) {
    // Pass a tagged buffer into the raw helper without letting IPA replace
    // its argument with the untagged global symbol.
    void *env = target;
    __asm__ volatile("" : "+r"(env));
    jump_from_boundary(1, env);
  }
  SIGNAL_CHECK(libc_state() == 0);
  if (setjmp(plain_target) == 0) {
    void *env = plain_target;
    __asm__ volatile("" : "+r"(env));
    jump_from_boundary(0, env);
  }
  SIGNAL_CHECK(libc_state() == 0);
  return 0;
}
