//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// glibc realloc(p, 0) frees p even though it returns null. Retire its ID
// instead of treating the null result as a failed allocation.
// RUN: %clang_ptsan -O2 -fno-builtin -o %t %s
// RUN: %run %t
// RUN: %clang_ptsan_id32 -O2 -fno-builtin -o %t.id32 %s
// RUN: %run %t.id32

#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stdlib.h>

int main(void) {
  void *p = malloc(32);
  assert(p != NULL);
  PTSanID id = get_id(p);
  assert(id != 0);
  assert(ptsan_get_size(id) == 32);

  void *result = realloc(p, 0);
  assert(result == NULL);
  assert(ptsan_get_size(id) == 0);
  return 0;
}
