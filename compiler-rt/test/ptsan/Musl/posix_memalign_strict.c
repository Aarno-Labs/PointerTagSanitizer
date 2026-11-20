//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -fno-builtin -o %t %s
// RUN: %run %t
// RUN: not %run %t bad-slot 2>&1 | FileCheck %s
// CHECK: Found bad write access: #0. Function ID: 0xdeadbeef

// The instrumented musl implementation must receive the caller's tagged
// output slot, not an untagged temporary in the runtime wrapper.
#include "../Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>

static void allocate(void **slot) {
  char sentinel;
  *slot = &sentinel;
  assert(posix_memalign(slot, 3, 123) == EINVAL);
  assert(*slot == &sentinel);
  assert(get_id(*slot) == get_id(&sentinel));

  assert(posix_memalign(slot, 64, 123) == 0);
  unsigned char *p = *slot;
  assert(p != NULL);
  assert(get_id(p) != 0 && get_id(p) != ptsan_default_id());
  assert(ptsan_get_size(get_id(p)) == 123);
  assert(((uintptr_t)p & 63) == 0);
  p[0] = 1;
  p[122] = 2;
  free(p);
}

int main(int argc, char **argv) {
  (void)argv;
  if (argc > 1) {
    void **small = malloc(1);
    assert(small != NULL);
    // The wrapper must reject the undersized output slot before libc writes.
    (void)posix_memalign(small, 64, 123);
    _Exit(0);
  }

  void *slot;
  allocate(&slot);
  void **heap_slot = malloc(sizeof(*heap_slot));
  assert(heap_slot != NULL);
  allocate(heap_slot);
  free(heap_slot);
  _Exit(0);
}
