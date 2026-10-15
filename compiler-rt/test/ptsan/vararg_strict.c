//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error -o %t.normal %s
// RUN: %t.normal
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error \
// RUN:   -DTEST_CASE=OVERFLOW_INT_OVERREAD -o %t.int-overread %s
// RUN: not %t.int-overread > %t.int.out 2>&1
// RUN: FileCheck %s --check-prefix=OVERREAD < %t.int.out
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error \
// RUN:   -DTEST_CASE=OVERFLOW_FP_OVERREAD -o %t.fp-overread %s
// RUN: not %t.fp-overread > %t.fp.out 2>&1
// RUN: FileCheck %s --check-prefix=OVERREAD < %t.fp.out
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error=0 \
// RUN:   -DTEST_CASE=EMPTY_OVERFLOW -o %t.empty %s
// RUN: %t.empty > %t.empty.out 2>&1
// RUN: FileCheck %s --check-prefix=RECOVER < %t.empty.out
//
// OVERREAD: Found bad {{.*}} access
// RECOVER: Found bad POINT access
// RECOVER: Unreclaimed IDs at exit: 0

// Exercise the four SysV x86-64 argument paths independently, then check
// copying/restarting lists, recursion, and indirect calls with mixed arguments.
// Overread cases run in ordinary mode so strict rejection of a valid zero-ID
// pointer cannot mask a missed overread. Register-only overreads remain XFAILs
// in separate drivers, since lit assigns XFAIL status to a whole test file.
#include <stdarg.h>

enum {
  VALID,
  OVERFLOW_INT_OVERREAD,
  OVERFLOW_FP_OVERREAD,
  REGISTER_INT_OVERREAD,
  REGISTER_FP_OVERREAD,
  EMPTY_OVERFLOW
};

#ifndef TEST_CASE
#define TEST_CASE VALID
#endif

static volatile int int_result;
static volatile double fp_result;

// Keep the readers and variadic functions externally visible and uninlined:
// specialization must not remove reads or change the fixed-argument ABI.
// The volatile stores preserve reads without depending on unsupplied values.
__attribute__((noinline)) void read_ints(va_list args, int count) {
  for (int i = 0; i < count; ++i) {
    int_result = va_arg(args, int);
  }
}

__attribute__((noinline)) void read_fps(va_list args, int count) {
  for (int i = 0; i < count; ++i) {
    fp_result = va_arg(args, double);
  }
}

__attribute__((noinline)) void register_int(int count, ...) {
  va_list args;
  va_start(args, count);
  read_ints(args, count);
  va_end(args);
}

__attribute__((noinline)) void register_fp(int count, ...) {
  va_list args;
  va_start(args, count);
  read_fps(args, count);
  va_end(args);
}

// Fixed arguments exhaust the relevant register class, so the first va_arg
// reads overflow storage rather than a register-save slot.
__attribute__((noinline)) void overflow_int(int a, int b, int c, int d, int e,
                                          int count, ...) {
  va_list args;
  va_start(args, count);
  read_ints(args, count);
  va_end(args);
}

__attribute__((noinline)) void overflow_fp(double a, double b, double c,
                                         double d, double e, double f,
                                         double g, double count, ...) {
  va_list args;
  va_start(args, count);
  read_fps(args, (int)count);
  va_end(args);
}

// Long double and the large struct require stack storage even when both
// register classes still have room: the sentinel must be memory-passed too.
struct Triple {
  long a, b, c;
};

__attribute__((noinline)) int consume(va_list arguments) {
  struct Triple triple = va_arg(arguments, struct Triple);
  long double fp = va_arg(arguments, long double);
  if (triple.a != 11 || triple.b != 22 || triple.c != 33 || fp != 4.5L) {
    return 1;
  }
  for (int i = 1; i <= 10; ++i) {
    if (va_arg(arguments, int) != i) {
      return 2;
    }
    if (va_arg(arguments, double) != i + 0.5) {
      return 3;
    }
  }
  return 0;
}

int variadic(int depth, ...);
int (*volatile indirect)(int, ...) = variadic;

__attribute__((noinline)) int invoke(int depth) {
  return indirect(depth, (struct Triple){11, 22, 33}, 4.5L,
                  1, 1.5, 2, 2.5, 3, 3.5, 4, 4.5, 5, 5.5,
                  6, 6.5, 7, 7.5, 8, 8.5, 9, 9.5, 10, 10.5);
}

__attribute__((noinline)) int variadic(int depth, ...) {
  va_list arguments, copy;
  va_start(arguments, depth);
  va_copy(copy, arguments);
  int result = consume(arguments);
  va_end(arguments);
  if (depth && invoke(depth - 1)) {
    return 4;
  }
  // Ending one list must not retire the backing IDs of its copy.
  result |= consume(copy);
  va_end(copy);
  va_start(arguments, depth);
  result |= consume(arguments);
  va_end(arguments);
  return result;
}

static int test_backing_storage(void) {
  for (int i = 0; i < 10; ++i) {
    if (invoke(3)) {
      return 1;
    }
  }
  return 0;
}

int main(void) {
  switch (TEST_CASE) {
  case OVERFLOW_INT_OVERREAD:
    overflow_int(0, 0, 0, 0, 0, 2, 42);
    return 0;
  case OVERFLOW_FP_OVERREAD:
    overflow_fp(0., 0., 0., 0., 0., 0., 0., 2., 42.5);
    return 0;
  case REGISTER_INT_OVERREAD:
    register_int(2, 42);
    return 0;
  case REGISTER_FP_OVERREAD:
    register_fp(2, 42.5);
    return 0;
  case EMPTY_OVERFLOW:
    // Read past the register-save slots with no supplied overflow arguments.
    register_int(32, 42);
    return 0;
  default:
    register_int(1, 42);
    if (int_result != 42) {
      return 1;
    }
    register_fp(1, 42.5);
    if (fp_result != 42.5) {
      return 2;
    }
    overflow_int(0, 0, 0, 0, 0, 1, 42);
    if (int_result != 42) {
      return 3;
    }
    overflow_fp(0., 0., 0., 0., 0., 0., 0., 1., 42.5);
    if (fp_result != 42.5) {
      return 4;
    }
    return test_backing_storage();
  }
}
