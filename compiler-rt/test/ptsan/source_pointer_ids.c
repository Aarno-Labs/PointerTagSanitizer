//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error -o %t %s
// RUN: %t 2>&1 | FileCheck %s --check-prefix=GOOD
// RUN: not %t bad 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: %clang_ptsan -O2 -mllvm -ptsan-no-opts \
// RUN:   -mllvm -ptsan-abort-on-error -o %t.noopts %s
// RUN: %t.noopts 2>&1 | FileCheck %s --check-prefix=GOOD
// RUN: not %t.noopts bad 2>&1 | FileCheck %s --check-prefix=BAD

// GOOD-NOT: Found bad
// GOOD: source-pointer-ids-ok
// GOOD-NOT: Found bad
// BAD: Found bad POINT access

// Mix direct stack accesses with escaping derived pointers. Every escape of
// one allocation must carry the same nonzero ID, including across loop
// iterations. Distinct live allocations must have distinct IDs. A returned
// pointer must retain its ID, so an out-of-bounds read through it is reported.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

__attribute__((noinline)) static unsigned id_of(const void *p) {
  return (uintptr_t)p >> 48;
}

__attribute__((noinline)) static char *identity(char *p) {
  return p;
}

__attribute__((noinline)) static char read_at(char *p, unsigned index) {
  return *(volatile char *)(p + index);
}

int main(int argc, char **argv) {
  char first[16] = {7};
  char second[16] = {9};
  unsigned first_id = id_of(first);
  assert(first_id && id_of(second) && first_id != id_of(second));
  for (unsigned i = 0; i != 16; ++i) {
    first[i] = (char)i;
    assert(id_of(first + i) == first_id);
    char *p = identity(first + i);
    assert(id_of(p) == first_id);
    assert(read_at(p, 0) == (char)i);
  }
  if (argc > 1) {
    return read_at(identity(first), 16);
  }
  puts("source-pointer-ids-ok");
  return 0;
}
