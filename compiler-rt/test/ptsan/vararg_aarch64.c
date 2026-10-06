//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: aarch64-target-arch
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error %s -o %t.bounds
// RUN: %run %t.bounds
// RUN: not %run %t.bounds gpr 2>&1 | FileCheck %s
// RUN: not %run %t.bounds fp 2>&1 | FileCheck %s
// RUN: not %run %t.bounds empty 2>&1 | FileCheck %s
// CHECK: Found bad {{.*}} access

// AAPCS64 has eight GP and eight FP argument registers. Exhaust each class
// explicitly for overflow tests; strict rejection of ID zero must not stand
// in for real bounds checking, hence the ordinary-mode overread executable.
// Valid cases cover both save areas, transitions to stack arguments, 16-byte
// alignment, homogeneous FP aggregates, indirectly passed large aggregates,
// forwarding/copying/restarting lists, and recursive/indirect calls.
#include <stdarg.h>

static volatile long integer_result;
static volatile double fp_result;

__attribute__((noinline)) void gp_overflow(long a, long b, long c, long d,
                                         long e, long f, long g, long count, ...) {
  va_list ap;
  va_start(ap, count);
  for (long i = 0; i < count; ++i)
    integer_result = va_arg(ap, long);
  va_end(ap);
}

__attribute__((noinline)) void fp_overflow(double a, double b, double c,
                                         double d, double e, double f,
                                         double g, double count, ...) {
  va_list ap;
  va_start(ap, count);
  for (int i = 0; i < (int)count; ++i)
    fp_result = va_arg(ap, double);
  va_end(ap);
}

struct Pair { double a, b; };
struct Large { long a, b, c; };

__attribute__((noinline)) int consume(va_list *ap) {
  struct Pair pair = va_arg(*ap, struct Pair);
  struct Large large = va_arg(*ap, struct Large);
  __int128 aligned = va_arg(*ap, __int128);
  long double wide = va_arg(*ap, long double);
  if (pair.a != 1.5 || pair.b != 2.5 || large.a != 11 || large.b != 22 ||
      large.c != 33 || aligned != ((__int128)1 << 80) + 7 || wide != 4.5L)
    return 1;
  for (int i = 1; i <= 12; ++i) {
    if (va_arg(*ap, long) != i || va_arg(*ap, double) != i + 0.5)
      return 2;
  }
  return 0;
}

int variadic(int depth, ...);
int (*volatile indirect)(int, ...) = variadic;

__attribute__((noinline)) int invoke(int depth) {
  return indirect(depth, (struct Pair){1.5, 2.5}, (struct Large){11, 22, 33},
                  ((__int128)1 << 80) + 7, 4.5L,
                  1L, 1.5, 2L, 2.5, 3L, 3.5, 4L, 4.5, 5L, 5.5, 6L, 6.5,
                  7L, 7.5, 8L, 8.5, 9L, 9.5, 10L, 10.5, 11L, 11.5, 12L, 12.5);
}

__attribute__((noinline)) int variadic(int depth, ...) {
  va_list ap, copy;
  va_start(ap, depth);
  va_copy(copy, ap);
  int result = consume(&ap);
  va_end(ap);
  if (depth && invoke(depth - 1))
    return 3;
  result |= consume(&copy);
  va_end(copy);
  va_start(ap, depth);
  result |= consume(&ap);
  va_end(ap);
  return result;
}

int main(int argc, char **argv) {
  if (argc > 1) {
    switch (argv[1][0]) {
    case 'g': gp_overflow(0, 0, 0, 0, 0, 0, 0, 2, 42L); break;
    case 'f': fp_overflow(0., 0., 0., 0., 0., 0., 0., 2., 42.5); break;
    case 'e': gp_overflow(0, 0, 0, 0, 0, 0, 0, 1); break;
    }
    return 0;
  }
  gp_overflow(0, 0, 0, 0, 0, 0, 0, 1, 42L);
  fp_overflow(0., 0., 0., 0., 0., 0., 0., 1., 42.5);
  if (integer_result != 42 || fp_result != 42.5)
    return 4;
  return invoke(3);
}
