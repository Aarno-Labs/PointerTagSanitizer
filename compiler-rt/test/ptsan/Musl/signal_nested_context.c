//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict %s -o %t
// RUN: %run %t
//
// SA_NODEFER permits deterministic same-signal nesting. Every live context,
// fixed alloca, and repeatable alloca must have a separate ID. Unwinding an
// inner handler must leave every outer object valid.
#define _GNU_SOURCE
#include "Inputs/signal_test.h"
#include <alloca.h>
#include <ucontext.h>

enum { Depth = 3, Objects = 4 };
static uintptr_t objects[Depth][Objects];
static volatile sig_atomic_t depth, calls;
static volatile unsigned dynamic_size = 43;

static void handler(int signo, siginfo_t *info, void *context) {
  unsigned level = depth++;
  SIGNAL_CHECK(level < Depth && signo == SIGUSR1);
  char local[37];
  unsigned size = dynamic_size;
  char *dynamic = alloca(size);
  signal_touch(local, sizeof(local));
  signal_touch(dynamic, size);
  objects[level][0] = (uintptr_t)info;
  objects[level][1] = (uintptr_t)context;
  objects[level][2] = (uintptr_t)local;
  objects[level][3] = (uintptr_t)dynamic;
  signal_check_live(info, sizeof(*info));
  signal_check_live(context, sizeof(ucontext_t));
  signal_check_live(local, sizeof(local));
  signal_check_live(dynamic, size);
  for (unsigned i = 0; i != Objects; ++i) {
    for (unsigned outer = 0; outer <= level; ++outer) {
      for (unsigned j = 0; j != (outer == level ? i : Objects); ++j) {
        SIGNAL_CHECK(get_id(objects[level][i]) !=
                           get_id(objects[outer][j]));
      }
    }
  }
  if (level + 1 != Depth) {
    SIGNAL_CHECK(raise(SIGUSR1) == 0);
    for (unsigned i = 0; i != Objects; ++i) {
      signal_check_retired(objects[level + 1][i]);
    }
  }
  SIGNAL_CHECK(info->si_signo == signo);
  signal_check_live(info, sizeof(*info));
  signal_check_live(context, sizeof(ucontext_t));
  signal_check_bytes(local, sizeof(local));
  signal_check_bytes(dynamic, size);
  ++calls;
  --depth;
}

int main(void) {
  struct sigaction action = {0};
  action.sa_sigaction = handler;
  action.sa_flags = SA_SIGINFO | SA_NODEFER;
  SIGNAL_CHECK(sigemptyset(&action.sa_mask) == 0);
  SIGNAL_CHECK(sigaction(SIGUSR1, &action, 0) == 0);
  SIGNAL_CHECK(raise(SIGUSR1) == 0);
  SIGNAL_CHECK(depth == 0 && calls == Depth);
  for (unsigned level = 0; level != Depth; ++level) {
    for (unsigned i = 0; i != Objects; ++i) {
      signal_check_retired(objects[level][i]);
    }
  }
  return 0;
}
