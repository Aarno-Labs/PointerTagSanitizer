//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Check the marked ID32 layout and additive payload on an interior pointer.
// Decoding must recover the allocation address, while a raw pointer remains
// ID zero in non-strict mode.

// RUN: %clang_ptsan_id32 -O2 -I%crt_src/include -o %t %s
// RUN: %run %t

#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

int main(void) {
  unsigned char *p = malloc(64);
  assert(p != NULL);

  uint32_t id = get_id(p);
  assert(ptsan_id_bits() == 32);
  assert(id > UINT32_C(0x80000000));
  uint32_t origin = ptsan_get_origin(id);
  assert(ptsan_get_payload(p) == origin);

  unsigned char *q = p + 17;
  assert(get_id(q) == id);
  assert(ptsan_get_payload(q) == (origin + 17));

  unsigned char *raw = (unsigned char *)(uintptr_t)ptsan_get_base(id);
  assert(get_id(raw) == 0);
  p[0] = 42;
  assert(raw[0] == 42);
  assert(ptsan_get_size(id) >= 64);

  free(p);
  return 0;
}
