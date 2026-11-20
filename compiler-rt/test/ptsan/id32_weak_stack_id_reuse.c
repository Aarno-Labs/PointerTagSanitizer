//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Disabling full stack temporal safety must still return runtime-managed
// stack IDs to the recycle queue. Check reuse by both stack and heap objects.

// RUN: %clang_ptsan_id32 -O1 -mllvm -ptsan-stack-temporal-safety=0 \
// RUN:   -mllvm -ptsan-id-reuse-distance=1 \
// RUN:   -mllvm -ptsan-randomize-ids=0 \
// RUN:   -mllvm -ptsan-no-stack-id-fast-path \
// RUN:   -mllvm -ptsan-instrument-globals=0 -DSTACK_TO_STACK -o %t-stack %s
// RUN: %clang_ptsan_id32 -O1 -mllvm -ptsan-stack-temporal-safety=0 \
// RUN:   -mllvm -ptsan-id-reuse-distance=1 \
// RUN:   -mllvm -ptsan-randomize-ids=0 \
// RUN:   -mllvm -ptsan-no-stack-id-fast-path \
// RUN:   -mllvm -ptsan-instrument-globals=0 -DSTACK_TO_HEAP -o %t-heap %s
// RUN: %run %t-stack
// RUN: %run %t-heap

#include "Inputs/id_reuse.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

static volatile int Sink;
static PTSanID ObservedID;

__attribute__((noinline)) static void consume(int *p) {
  ++*p;
  Sink = *p;
}

__attribute__((noinline)) static void use_stack(void) {
  int value = 0;
  ObservedID = get_id(&value);
  consume(&value);
}

int main(void) {
  // Weak stack allocations must retire their IDs too. Batching can delay
  // reuse, so observe it rather than assuming a particular allocation order.
  use_stack();
  PTSanID first = ObservedID;
  assert(first != 0);

#ifdef STACK_TO_STACK
  unsigned i;
  for (i = 0; i != 32768; ++i) {
    use_stack();
    if (ObservedID == first) {
      break;
    }
  }
  assert(i != 32768);
#else
  int *p = allocate_reusing_id(first);
  assert(p);
  *p = 1;
  Sink = *p;
  free(p);
#endif

  assert(Sink == 1);
  return 0;
}
