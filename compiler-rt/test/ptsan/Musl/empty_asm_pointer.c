//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Empty register asm is a compiler barrier, not a raw-address boundary: its
// outputs must retain IDs and bounds. Real asm and indirect memory operands
// still need raw addresses. Exercise both, including an out-of-bounds access
// through a separate translation unit so lost bounds cannot go unnoticed.
// REQUIRES: x86_64-target-arch
// RUN: split-file %s %t.dir
// RUN: %ptsan_musl_strict -I%S/.. -O2 -mllvm -ptsan-abort-on-error %t.dir/main.c %t.dir/access.c -o %t
// RUN: %run %t
// RUN: not %run %t bad 2>&1 | FileCheck %s

// Empty asm must preserve ID32 metadata while real asm receives raw addresses.
// RUN: %ptsan_musl_id32_strict -I%S/.. %t.dir/main.c %t.dir/access.c -o %t.id32
// RUN: %run %t.id32
// RUN: not %run %t.id32 bad 2>&1 | FileCheck %s
// CHECK: Found bad POINT access

//--- main.c
#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stdlib.h>

int read_offset(const int *, unsigned);

int main(int argc, char **argv) {
  int *p = malloc(4 * sizeof(int));
  int *q = malloc(4 * sizeof(int));
  assert(p && q);
  p[0] = 17;
  q[0] = 29;
  unsigned p_id = get_id(p), q_id = get_id(q);
  assert(p_id && q_id && p_id != q_id);

  // Protobuf uses this on an incremented table pointer to discourage CSE.
  int *end = p + 4;
  __asm__("" : "+r"(end));
  assert(get_id(end) == p_id);
  assert(ptsan_get_address(end) == ptsan_get_address(p) + 4 * sizeof(int));
  assert(read_offset(end - 4, 0) == 17);

  // A fixed-register constraint also preserves the pointer representation.
  __asm__("" : "+a"(p));
  assert(get_id(p) == p_id);
  assert(read_offset(p, 0) == 17);

  // Multiple tied registers and a memory clobber still execute no instructions.
  __asm__ volatile("" : "+r"(p), "+r"(q) : : "memory");
  assert(get_id(p) == p_id && get_id(q) == q_id);
  assert(read_offset(p, 0) == 17 && read_offset(q, 0) == 29);

  // Indirect memory operands must retain their raw-address handling even when
  // another operand in the same empty asm is a tied pointer register.
  __asm__ volatile("" : "+r"(p), "+m"(*q));
  assert(get_id(p) == p_id);
  assert(read_offset(q, 0) == 29);

  // Nonempty asm must still see the raw pointer, even with a tied register.
  int *raw = p;
  __asm__("testq %0, %0" : "+r"(raw) : : "cc");
  // Raw addresses cannot be classified with get_id in markerless mode.
  // Compare all native address bits instead.
  assert((uintptr_t)raw == ptsan_get_address(p));

  if (argc > 1) {
    (void)read_offset(end, 0);
    assert(0 && "empty asm must not lose the bounds check");
  }
  free(p);
  free(q);
}

//--- access.c
int read_offset(const int *p, unsigned index) {
  return ((const volatile int *)p)[index];
}
