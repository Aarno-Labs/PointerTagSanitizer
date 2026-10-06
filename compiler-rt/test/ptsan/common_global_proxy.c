//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: split-file %s %t.split
// RUN: %clang_ptsan -O2 -fcommon -I%S -o %t.common \
// RUN:   %t.split/main.c %t.split/first.c %t.split/second.c
// RUN: %run %t.common 2>&1 | FileCheck %s
// RUN: %clang_ptsan -O2 -fcommon -I%S -o %t.reverse \
// RUN:   %t.split/second.c %t.split/first.c %t.split/main.c
// RUN: %run %t.reverse 2>&1 | FileCheck %s
// RUN: %clang_ptsan -O2 -fcommon -DSTRONG -I%S -o %t.strong \
// RUN:   %t.split/main.c %t.split/first.c %t.split/second.c
// RUN: %run %t.strong 2>&1 | FileCheck %s
// RUN: %clang_ptsan -O2 -fcommon -DSTRONG -I%S -o %t.strong_reverse \
// RUN:   %t.split/second.c %t.split/first.c %t.split/main.c
// RUN: %run %t.strong_reverse 2>&1 | FileCheck %s

// Common definitions must share a tagged proxy across translation units. A
// strong definition must override them without creating a different proxy.
// Exercise both link orders, including an extern-only consumer.
// CHECK-NOT: Found bad
// CHECK: common-global-proxy-ok
// CHECK-NOT: Found bad

//--- main.c
#include "Inputs/ptsan_test_interface.h"
#include <assert.h>

extern int Shared;
int *first_address(void);
int *second_address(void);
int read_shared(const volatile int *p);

int main(void) {
  int *first = first_address();
  int *second = second_address();
  uint16_t id = get_id(first);
  assert(id != 0 && id != ptsan_default_id());
  assert(id == get_id(second) && id == get_id(&Shared));
  // Integer equality checks the whole tagged representation, not just the
  // address bits of a pointer comparison.
  assert((uintptr_t)first == (uintptr_t)second);
  assert((uintptr_t)first == (uintptr_t)&Shared);
  assert(ptsan_get_size(id) == sizeof(Shared));
#ifdef STRONG
  assert(read_shared(first) == 42);
#else
  assert(read_shared(first) == 0);
#endif
  *first = 17;
  assert(read_shared(second) == 17);
  assert(read_shared(&Shared) == 17);
  puts("common-global-proxy-ok");
  return 0;
}

//--- first.c
int Shared;

int *first_address(void) {
  return &Shared;
}

int read_shared(const volatile int *p) {
  return *p;
}

//--- second.c
#ifdef STRONG
int Shared = 42;
#else
int Shared;
#endif

int *second_address(void) {
  return &Shared;
}
