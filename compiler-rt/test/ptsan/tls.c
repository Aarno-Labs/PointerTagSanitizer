//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: split-file %s %t
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error -o %t/ordinary \
// RUN:   %t/main.c %t/access.c
// RUN: %t/ordinary
// RUN: %clang_ptsan -O0 \
// RUN:   -mllvm -ptsan-abort-on-error -o %t/o0 %t/main.c %t/access.c
// RUN: %t/o0
// CHECK: Found bad POINT access

// Separate translation units keep accesses opaque and verify that an unsized
// external declaration uses the defining module's ID and object size.
//--- main.c
#include <assert.h>
#include <stdint.h>

_Thread_local int values[4] = {3, 7, 11, 13};
static _Thread_local int other = 19;
int *get_values(void);
int read_element(volatile int *p, int index);
void write_element(volatile int *p, int index, int value);

// Eager initialization must finish before ordinary application constructors.
__attribute__((constructor)) static void check_initialized(void) {
  uintptr_t id = (uintptr_t)get_values() >> 48;
  assert(id != 0 && id != 0xffff);
}

int main(int argc, char **argv) {
  int *p = get_values();
  uintptr_t id = (uintptr_t)p >> 48;
  assert(id != 0 && id != 0xffff);
  assert(id == (uintptr_t)values >> 48);
  assert(id == (uintptr_t)get_values() >> 48);
  assert(id != (uintptr_t)&other >> 48);
  assert(read_element(p, 0) == 3);
  assert(read_element(p, 3) == 13);
  write_element(p, 1, 23);
  assert(read_element(get_values(), 1) == 23);
  if (argc > 1) {
    // Test both ends through opaque accesses, not static-bound proofs.
    if (argc == 2) {
      return read_element(p, -1);
    }
    write_element(p, 4, 99);
  }
  return 0;
}

//--- access.c
extern _Thread_local int values[];

int *get_values(void) {
  return values;
}

int read_element(volatile int *p, int index) {
  return p[index];
}

void write_element(volatile int *p, int index, int value) {
  p[index] = value;
}
