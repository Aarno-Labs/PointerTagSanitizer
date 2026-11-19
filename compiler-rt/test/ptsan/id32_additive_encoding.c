//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Heap, global, and stack pointers must encode payload_start + byte offset
// and preserve raw alignment. Also check the deterministic weak-stack origin
// and retention of decode metadata after an object's lifetime ends. Random
// salts must leave at least 4 KiB of underflow without reserving the first
// bit above their alignment for bookkeeping.

// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-random-seed=1 -I%crt_src/include -o %t %s
// RUN: %run %t
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-random-seed=1 -mllvm -ptsan-stack-temporal-safety=0 -DWEAK_STACK -I%crt_src/include -o %t-weak %s
// RUN: %run %t-weak

#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

char Global[8];

extern uint32_t ptsan_choose_stack_origin(uint64_t);
static uint64_t preserved_alignment;

static void check_requested_alignment(void) {
  bool saw_even = false;
  bool saw_odd = false;
  for (uintptr_t alignment = 1; alignment <= (1U << 30); alignment *= 2) {
    uint64_t quantum =
        alignment > preserved_alignment ? alignment : preserved_alignment;
    uint32_t salt = ptsan_choose_stack_origin(alignment);
    assert(salt >= 4096);
    assert(salt < UINT64_C(0x80000000));
    assert(salt % quantum == 0);
    saw_even |= (salt / quantum) % 2 == 0;
    saw_odd |= (salt / quantum) % 2 != 0;
  }
  assert(saw_even && saw_odd);
  // At this quantum zero would occur frequently without rejection sampling.
  for (unsigned i = 0; i < 4096; ++i) {
    uint32_t salt = ptsan_choose_stack_origin(2 * 1024 * 1024);
    assert(salt >= 2 * 1024 * 1024);
  }
}

static void check_heap_origin(const void *p, uint32_t id) {
  uint64_t base = ptsan_get_base(id);
  uint64_t span = ptsan_get_size(id);
  uint64_t origin = ptsan_get_origin(id);
  uint64_t salt = origin - base % preserved_alignment;
  assert(salt >= 4096);
  assert(salt % preserved_alignment == 0);
  assert(origin <= UINT32_MAX - span);
  assert(origin < UINT64_C(0x80000000));
  assert((origin & (preserved_alignment - 1)) ==
         (base & (preserved_alignment - 1)));
  assert((ptsan_get_payload(p) & (preserved_alignment - 1)) ==
         (base & (preserved_alignment - 1)));
  assert((ptsan_get_payload((const char *)p + 7) &
          (preserved_alignment - 1)) ==
         ((base + 7) & (preserved_alignment - 1)));
}

__attribute__((noinline)) static uint32_t check_stack(uint32_t *saved_origin) {
  _Alignas(16) volatile char local[8];
  local[0] = 1;

  uint32_t id = get_id((const void *)local);
  uint32_t origin = ptsan_get_origin(id);
  *saved_origin = origin;
  assert(origin >= 4096);
  assert(origin <= UINT64_C(0x80000000));
  assert(ptsan_get_payload((const void *)local) == origin);
  assert((origin & (preserved_alignment - 1)) ==
         (ptsan_get_address((const void *)local) & (preserved_alignment - 1)));
#ifdef WEAK_STACK
  assert(origin == preserved_alignment +
                       ptsan_get_address((const void *)local) % preserved_alignment);
#endif
  return id;
}

int main(void) {
  preserved_alignment = (uint64_t)sysconf(_SC_PAGESIZE);
  check_requested_alignment();

  Global[0] = 1;
  assert(Global[0] == 1);

  uint32_t global_id = get_id(Global);
  assert(global_id != 0);
  uint32_t global_origin = ptsan_get_origin(global_id);
  assert(global_origin >= 4096);
  assert(global_origin <= UINT64_C(0x80000000));
  assert(global_origin % preserved_alignment ==
         ptsan_get_address(Global) % preserved_alignment);
  assert(ptsan_get_payload(Global) == global_origin);

  char *p = malloc(32);
  assert(p != NULL);
  uint32_t id = get_id(p);
  uint32_t origin = ptsan_get_origin(id);
  check_heap_origin(p, id);
  assert(ptsan_get_payload(p) == origin);
  assert(ptsan_get_payload(p + 7) == (origin + 7));
  p[7] = 42;
  assert(p[7] == 42);
  free(p);
  assert(ptsan_get_origin(id) == origin);

  uint32_t stack_origin;
  uint32_t stack_id = check_stack(&stack_origin);
  assert(ptsan_get_origin(stack_id) == stack_origin);
  return 0;
}
