//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//
// OMNeT++ measures stack consumption by subtracting pointers to objects in
// different stack frames. This is not defined C, but clear-IDs-on-arithmetic
// exists to support programs which rely on the machine-address result.
//
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-stack-temporal-safety=0 \
// RUN:   -mllvm -ptsan-clear-ids-on-cmp=0 \
// RUN:   -mllvm -ptsan-clear-ids-on-arith=1 -o %t.clear %s
// RUN: %run %t.clear

#include "Inputs/ptsan_test_interface.h"
#include <assert.h>

struct Frame {
  char Storage[464];
};

__attribute__((noinline)) static unsigned distance(struct Frame *A,
                                                   struct Frame *B) {
  char *CA = (char *)A;
  char *CB = (char *)B;
  return (unsigned)(CA > CB ? CA - CB : CB - CA);
}

__attribute__((noinline)) static unsigned inner(struct Frame *Outer) {
  struct Frame Inner;
  Inner.Storage[0] = 1;
  uint64_t outer_address = ptsan_get_address(Outer);
  uint64_t inner_address = ptsan_get_address(&Inner);
  uint64_t expected = outer_address > inner_address
                          ? outer_address - inner_address
                          : inner_address - outer_address;
  unsigned result = distance(Outer, &Inner);
  assert(expected != 0 && expected <= UINT32_MAX);
  assert(result == expected);
  return result;
}

int main(void) {
  struct Frame Outer;
  Outer.Storage[0] = 1;
  inner(&Outer);
  return 0;
}
