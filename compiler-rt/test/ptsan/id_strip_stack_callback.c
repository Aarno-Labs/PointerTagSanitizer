//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: split-file %s %t.dir
// RUN: %clang_strip -O2 \
// RUN:   -c %t.dir/helper.c -o %t.helper.o
// RUN: %clang_ptsan -O2 -I%S %t.dir/main.c %t.helper.o -o %t
// RUN: %run %t

// RUN: %clang_strip -fsanitize-ptsan-32-bit-ids -DPTSAN_TEST_32_BIT_IDS -O2 \
// RUN:   -c %t.dir/helper.c -o %t.helper.o
// RUN: %clang_ptsan_id32 -O2 -I%S %t.dir/main.c %t.helper.o -o %t
// RUN: %run %t

// Strip-only libraries do not allocate stack IDs. Passing their local storage
// back into instrumented code must preserve its raw address, with ID zero.

//--- helper.c
#include <stddef.h>
#include <stdint.h>

void check_local(char *, uintptr_t);

void visit_locals(size_t size) {
  char fixed[16];
  char dynamic[size];
  fixed[0] = dynamic[0] = 7;
  check_local(fixed, (uintptr_t)fixed);
  check_local(dynamic, (uintptr_t)dynamic);
}

//--- main.c
#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

void visit_locals(size_t);

void check_local(char *pointer, uintptr_t raw_address) {
  assert(get_id(pointer) == 0);
  assert((uintptr_t)pointer == raw_address);
  assert(*(volatile char *)pointer == 7);
  *(volatile char *)pointer = 9;
}

int main(void) {
  visit_locals(32);
  return 0;
}
