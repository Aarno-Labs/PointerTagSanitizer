//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -DUSE_LIBCALL \
// RUN:   -fno-builtin-frexp -o %t.direct %s
// RUN: %run %t.direct
// RUN: %ptsan_musl_strict -S -emit-llvm \
// RUN:   -o - %s | FileCheck %s --check-prefix=IR
// RUN: %ptsan_musl_strict -S -o - %s | \
// RUN:   FileCheck %s --check-prefix=ASM
// IR: call { double, i32 } @llvm.frexp.f64.i32
// ASM-LABEL: split:
// ASM: {{callq|bl}} {{.*}}frexp

// The intrinsic returns its exponent as an SSA value. Code generation creates
// a stack temporary and passes its raw address to musl's frexp implementation.
// In contrast, a real C call passes the instrumented caller's exponent object.
// The strict intrinsic case is isolated in frexp_libcall_strict_xfail.c.

#include <math.h>
#include <stdlib.h>

__attribute__((noinline)) static double split(double value, int *exponent) {
#ifdef USE_LIBCALL
  return frexp(value, exponent);
#else
  return __builtin_frexp(value, exponent);
#endif
}

int main(void) {
  // Prevent constant folding without involving any other libc calls.
  volatile double input = 6.5;
  int exponent;
  double fraction = split(input, &exponent);
  if (fraction != 0.8125 || exponent != 3)
    _Exit(2);
  // Avoid the unrelated strict-mode exit/TLS issue.
  _Exit(0);
}
