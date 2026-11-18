//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -o %t %s
// RUN: %run %t
// RUN: %ptsan_musl_id32_strict -Wall -Wextra -Werror -o %t.id32 %s
// RUN: %run %t.id32

#include <math.h>
#include <stdlib.h>

__attribute__((noinline)) static float split_float(float value,
                                                   float *integer) {
  return modff(value, integer);
}

__attribute__((noinline)) static double split_double(double value,
                                                     double *integer) {
  return modf(value, integer);
}

__attribute__((noinline)) static long double
split_long_double(long double value, long double *integer) {
  return modfl(value, integer);
}

int main(void) {
  volatile float float_input = -6.5f;
  volatile double double_input = -6.5;
  volatile long double long_double_input = -6.5L;
  float float_integer;
  double double_integer;
  long double long_double_integer;

  if (split_float(float_input, &float_integer) != -0.5f ||
      float_integer != -6.0f ||
      split_double(double_input, &double_integer) != -0.5 ||
      double_integer != -6.0 ||
      split_long_double(long_double_input, &long_double_integer) != -0.5L ||
      long_double_integer != -6.0L) {
    _Exit(2);
  }
  _Exit(0);
}
