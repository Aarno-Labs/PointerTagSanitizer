//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error -o %t %s
// RUN: %t 2>&1 | FileCheck %s
// RUN: %clang_ptsan -O2 -mllvm -ptsan-no-opts \
// RUN:   -mllvm -ptsan-abort-on-error -o %t.noopts %s
// RUN: %t.noopts 2>&1 | FileCheck %s

// CHECK-NOT: Found bad
// CHECK: va-list-source-ids-ok
// CHECK-NOT: Found bad

// va_start/va_copy/va_end need raw storage addresses, while a forwarded list
// needs its allocation ID. Verify both representations work together. This
// tests the list storage, not bounds on the ABI-managed argument areas.
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

__attribute__((noinline)) static unsigned id_of(const void *p) {
  return (uintptr_t)p >> 48;
}

__attribute__((noinline)) static void consume(va_list *arguments) {
  assert(id_of(arguments));
  va_list copy;
  va_copy(copy, *arguments);
  assert(id_of(&copy) && id_of(&copy) != id_of(arguments));
  for (int i = 1; i <= 10; ++i) {
    assert(va_arg(*arguments, int) == i);
    assert(va_arg(copy, int) == i);
  }
  va_end(copy);
}

__attribute__((noinline)) static void variadic(int dummy, ...) {
  va_list arguments;
  va_start(arguments, dummy);
  consume(&arguments);
  va_end(arguments);
}

int main(void) {
  variadic(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
  puts("va-list-source-ids-ok");
  return 0;
}
