//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Musl's setjmp entry points are assembly routines whose direct calls are
// stripped by the compiler. The longjmp family uses wrappers because those
// ordinary non-returning functions may also be called indirectly.
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t

#define _GNU_SOURCE
#include <setjmp.h>

typedef void (*LongjmpFunction)(jmp_buf, int);

static __attribute__((noinline, noreturn)) void
jumpIndirectly(LongjmpFunction function, jmp_buf environment, int value) {
  function(environment, value);
  __builtin_unreachable();
}

int main(void) {
  jmp_buf environment;
  int value = setjmp(environment);
  if (value == 0) {
    jumpIndirectly(longjmp, environment, 11);
  }
  if (value != 11) {
    return 1;
  }

  jmp_buf fast_environment;
  value = _setjmp(fast_environment);
  if (value == 0) {
    jumpIndirectly(_longjmp, fast_environment, 12);
  }
  if (value != 12) {
    return 2;
  }

  sigjmp_buf signal_environment;
  value = sigsetjmp(signal_environment, 0);
  if (value == 0) {
    jumpIndirectly(siglongjmp, signal_environment, 13);
  }
  if (value != 13) {
    return 3;
  }

  return 0;
}
