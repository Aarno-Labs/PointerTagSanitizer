//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: split-file %s %t.dir
// RUN: %clang_raw -O2 -DPTSAN_TEST_32_BIT_IDS -I%S -c %t.dir/inspect.c -o %t.inspect.o
// RUN: %clang_ptsan_id32 -O2 %t.dir/main.c %t.inspect.o -o %t
// RUN: %run %t

// An ordinary 32-byte allocation in Fleetbench RPC landed at this 32-GiB-aligned
// address. Registration used to mistake incidental alignment for a requirement
// and abort because no origin fits. No large alignment was requested.
// Register the captured address synthetically; do not dereference or free it.
//--- inspect.c
#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stdint.h>
#include <unistd.h>

extern uint32_t ptsan_get_next_nonstack_id(void *, uint64_t, uint8_t, uint64_t);
extern void ptsan_resize_buffer(uint32_t, uint64_t);

void check_registration(void) {
  const uint64_t base = UINT64_C(0x13b800000000);
  const uint64_t size = 32;
  const uint8_t malloc_kind = 1;
  uint32_t id = ptsan_get_next_nonstack_id((void *)base, size, malloc_kind, 16);
  assert(id != 0 && id != ptsan_default_id());
  uint64_t origin = ptsan_get_origin(id);
  uint64_t page_size = (uint64_t)sysconf(_SC_PAGESIZE);
  assert(origin < UINT64_C(0x80000000));
  assert(origin % page_size == base % page_size);
  void *encoded = (void *)(((uint64_t)id << 32) | origin);
  assert(ptsan_get_id(encoded) == id);
  assert(ptsan_get_address(encoded) == base);
  assert(ptsan_get_size(id) == size);
  assert(((uintptr_t)encoded & 15) == 0);

  // Growth and shrinkage leave both the origin and the encoded pointer alone,
  // regardless of the raw address's incidental alignment.
  const uint64_t sizes[] = {UINT64_C(0x7fffffff), 16, 8192};
  for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
    ptsan_resize_buffer(id, sizes[i]);
    assert(ptsan_get_origin(id) == origin);
    assert(ptsan_get_address(encoded) == base);
    assert(ptsan_get_id(encoded) == id);
    assert(ptsan_get_size(id) == sizes[i]);
  }
}

//--- main.c
#include <stdint.h>
void check_registration(void);

int main(void) {
  check_registration();
  return 0;
}
