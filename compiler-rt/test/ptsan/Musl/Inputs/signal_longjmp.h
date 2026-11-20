//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// JUMP_TARGET: 0 escapes the outer handler; 1 jumps within that handler;
// 2 jumps from a nested handler back into the still-live outer handler.
#define _GNU_SOURCE
#include "signal_test.h"
#include <alloca.h>
#include <setjmp.h>
#include <ucontext.h>

static sigjmp_buf outside;
static volatile sig_atomic_t save_mask;
#if JUMP_TARGET != 0
static sigjmp_buf inside;
static volatile sig_atomic_t completed;
#endif
static volatile unsigned dynamic_size = 43, dynamic_count = 17;
static uintptr_t outer_objects[4], abandoned[2];
#if JUMP_TARGET == 2
static uintptr_t inner_objects[3];
#endif

__attribute__((noinline, noreturn)) static void
jump_from_child(sigjmp_buf env) {
  char fixed[37];
  signal_touch(fixed, sizeof(fixed));
  abandoned[0] = (uintptr_t)fixed;
  for (unsigned i = 0; i != dynamic_count; ++i) {
    unsigned n = dynamic_size;
    char *p = alloca(n);
    signal_touch(p, n);
    abandoned[1] = (uintptr_t)p;
  }
  sigset_t changed;
  SIGNAL_CHECK(sigemptyset(&changed) == 0);
  SIGNAL_CHECK(sigaddset(&changed, SIGWINCH) == 0);
  SIGNAL_CHECK(sigprocmask(SIG_BLOCK, &changed, 0) == 0);
  siglongjmp(env, 42);
}

#if JUMP_TARGET == 2
static void inner_handler(int signo, siginfo_t *info, void *context) {
  char local[37];
  SIGNAL_CHECK(signo == SIGUSR2);
  SIGNAL_CHECK(signal_in_signal_context() == 1);
  signal_touch(local, sizeof(local));
  signal_check_live(info, sizeof(*info));
  signal_check_live(context, sizeof(ucontext_t));
  inner_objects[0] = (uintptr_t)info;
  inner_objects[1] = (uintptr_t)context;
  inner_objects[2] = (uintptr_t)local;
  jump_from_child(inside);
}
#endif

static void outer_handler(int signo, siginfo_t *info, void *context) {
  char local[37];
  unsigned n = dynamic_size;
  char *retained = alloca(n);
  signal_touch(local, sizeof(local));
  signal_touch(retained, n);
  SIGNAL_CHECK(signo == SIGUSR1);
  SIGNAL_CHECK(signal_in_signal_context() == 1);
  signal_check_live(info, sizeof(*info));
  signal_check_live(context, sizeof(ucontext_t));
  outer_objects[0] = (uintptr_t)info;
  outer_objects[1] = (uintptr_t)context;
  outer_objects[2] = (uintptr_t)local;
  outer_objects[3] = (uintptr_t)retained;
#if JUMP_TARGET == 0
  jump_from_child(outside);
#else
  int result = sigsetjmp(inside, save_mask);
  if (result == 0) {
#  if JUMP_TARGET == 1
    jump_from_child(inside);
#  else
    SIGNAL_CHECK(raise(SIGUSR2) == 0);
    SIGNAL_CHECK(0 && "inner handler must jump");
#  endif
  }
  SIGNAL_CHECK(result == 42);
  SIGNAL_CHECK(signal_in_signal_context() == 1);
  // The target frame's alloca predates setjmp and must remain usable.
  signal_check_bytes(local, sizeof(local));
  signal_check_bytes(retained, n);
  signal_check_live(info, sizeof(*info));
  signal_check_live(context, sizeof(ucontext_t));
  SIGNAL_CHECK(info->si_signo == SIGUSR1);
  signal_check_retired(abandoned[0]);
  signal_check_retired(abandoned[1]);
#  if JUMP_TARGET == 2
  for (unsigned i = 0; i != 3; ++i) {
    signal_check_retired(inner_objects[i]);
  }
#  endif
  sigset_t current;
  SIGNAL_CHECK(sigprocmask(SIG_SETMASK, 0, &current) == 0);
  SIGNAL_CHECK(sigismember(&current, SIGUSR1) == 1);
  SIGNAL_CHECK(sigismember(&current, SIGWINCH) == !save_mask);
#  if JUMP_TARGET == 2
  SIGNAL_CHECK(sigismember(&current, SIGUSR2) == !save_mask);
#  endif
  completed = 1;
#endif
}

int main(int argc, char **argv) {
  SIGNAL_CHECK(argc == 2 && (argv[1][0] == '0' || argv[1][0] == '1'));
  save_mask = argv[1][0] == '1';
  sigset_t empty, original;
  SIGNAL_CHECK(sigemptyset(&empty) == 0);
  SIGNAL_CHECK(sigprocmask(SIG_SETMASK, &empty, &original) == 0);
  struct sigaction action = {0};
  action.sa_sigaction = outer_handler;
  action.sa_flags = SA_SIGINFO;
  SIGNAL_CHECK(sigemptyset(&action.sa_mask) == 0);
  SIGNAL_CHECK(sigaction(SIGUSR1, &action, 0) == 0);
#if JUMP_TARGET == 2
  action.sa_sigaction = inner_handler;
  SIGNAL_CHECK(sigaction(SIGUSR2, &action, 0) == 0);
#endif
  char fixed[37];
  unsigned n = dynamic_size;
  char *retained = alloca(n);
  signal_touch(fixed, sizeof(fixed));
  signal_touch(retained, n);
  uint64_t limit = signal_stack_limit();
  uint64_t live = signal_live_stack_ids();
  int result = sigsetjmp(outside, save_mask);
  if (result == 0) {
    SIGNAL_CHECK(raise(SIGUSR1) == 0);
  }
#if JUMP_TARGET == 0
  SIGNAL_CHECK(result == 42);
  SIGNAL_CHECK(signal_in_signal_context() == 0);
  signal_check_retired(abandoned[0]);
  signal_check_retired(abandoned[1]);
#else
  SIGNAL_CHECK(result == 0 && completed == 1);
  SIGNAL_CHECK(signal_in_signal_context() == 0);
#endif
  SIGNAL_CHECK(signal_stack_limit() == limit);
  SIGNAL_CHECK(signal_live_stack_ids() == live);
  signal_check_bytes(fixed, sizeof(fixed));
  signal_check_bytes(retained, n);
  for (unsigned i = 0; i != 4; ++i) {
    signal_check_retired(outer_objects[i]);
  }
  sigset_t current;
  SIGNAL_CHECK(sigprocmask(SIG_SETMASK, 0, &current) == 0);
  SIGNAL_CHECK(sigismember(&current, SIGUSR1) ==
               (JUMP_TARGET == 0 && !save_mask));
  SIGNAL_CHECK(sigismember(&current, SIGWINCH) ==
               (JUMP_TARGET == 0 && !save_mask));
  SIGNAL_CHECK(sigprocmask(SIG_SETMASK, &original, 0) == 0);
  return 0;
}
