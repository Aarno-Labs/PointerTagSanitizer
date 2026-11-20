//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: ptsan-instrumented-musl
// RUN: split-file %s %t
// RUN: %ptsan_musl_id32_strict -O2 -mllvm -ptsan-randomize-ids=0 -I%S/.. %t/main.c %t/access.c -o %t/low
// RUN: %run %t/low
// RUN: %ptsan_musl_id32_strict -O2 -mllvm -ptsan-randomize-ids=0 -mllvm -ptsan-stack-id-thread-buckets=128 -mllvm -ptsan-id-bucket-size=16777216 -DHIGH_IDS -I%S/.. %t/main.c %t/access.c -o %t/high
// RUN: %run %t/high
// RUN: not %run %t/high oob 2>&1 | FileCheck %s
// RUN: not %run %t/high stale extra 2>&1 | FileCheck %s
// CHECK: Found bad POINT access

// Force the allocator above bit 31 through reserved stack IDs, without
// allocating billions of objects or committing the metadata reservation.
//--- main.c
#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stdlib.h>

static int global = 19;
int read_value(const volatile int *p);

int main(int argc, char **argv) {
  (void)argv;
  assert(ptsan_id_bits() == 32);
  assert(ptsan_default_id() == UINT32_MAX);
  assert(ptsan_get_size(0) == 0);
  assert(ptsan_get_size(UINT32_MAX) == 0);
  int *p = malloc(2 * sizeof(*p));
  assert(p);
#ifdef HIGH_IDS
  assert(get_id(p) > INT32_MAX);
  assert(get_id(&global) > INT32_MAX);
#else
  assert(get_id(p) < INT32_MAX);
  assert(((uintptr_t)p >> 63) == 0);
#endif
  assert((uint32_t)((uintptr_t)p >> 32) == get_id(p));
  p[0] = 23;
  p[1] = 42;
  assert(read_value(&global) == 19);
  assert(read_value(p + 1) == 42);
  if (argc == 2)
    return read_value(p + 2);
  free(p);
  if (argc > 2)
    return read_value(p);
  return 0;
}

//--- access.c
int read_value(const volatile int *p) { return *p; }
