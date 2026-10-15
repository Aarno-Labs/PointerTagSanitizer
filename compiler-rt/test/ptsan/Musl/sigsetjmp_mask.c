//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -O0 %s -o %t
// RUN: %run %t
// RUN: %ptsan_musl_strict -O2 %s -o %t.opt
// RUN: %run %t.opt

// The assembly passes a raw jmp_buf to __sigsetjmp_tail on both returns.
// Its runtime wrapper must save/restore the mask without requiring a pointer
// ID, while sigsetjmp(..., 0) must leave the changed mask alone.
#include <assert.h>
#include <setjmp.h>
#include <signal.h>

__attribute__((noinline)) static void exercise(int save_mask) {
  sigset_t signal, original, current;
  assert(sigemptyset(&signal) == 0);
  assert(sigaddset(&signal, SIGUSR1) == 0);
  assert(sigprocmask(SIG_BLOCK, &signal, &original) == 0);

  sigjmp_buf env;
  if (sigsetjmp(env, save_mask) == 0) {
    assert(sigprocmask(SIG_UNBLOCK, &signal, 0) == 0);
    siglongjmp(env, 1);
  }
  assert(sigprocmask(SIG_SETMASK, 0, &current) == 0);
  assert(sigismember(&current, SIGUSR1) == save_mask);
  assert(sigprocmask(SIG_SETMASK, &original, 0) == 0);
}

int main(void) {
  exercise(0);
  exercise(1);
  return 0;
}
