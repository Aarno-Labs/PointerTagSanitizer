//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
#include "ptsan_test_interface.h"
#include <assert.h>

extern void ptsan_get_stack_ids(uint16_t *, uint64_t);
extern void ptsan_free_stack_ids(uintptr_t);

// Raw buffers exercise the runtime ABI directly. Exhaustion can produce mixed
// batches; cleanup must return their unique IDs and skip their default entries.
// Keep the separate loop-hoisting pass out of this raw runtime-ABI test.
__attribute__((disable_sanitizer_instrumentation, noinline, optnone))
int main(void) {
  enum { Recycled = 8192 };
  uint16_t live[65536], mixed[65537];
  uintptr_t cursors[65536];
  uintptr_t start = ptsan_stack_id_log.cursor;
  unsigned char owned[65536] = {0};
  unsigned count = 0;
  while (count != 65536) {
    cursors[count] = ptsan_stack_id_log.cursor;
    ptsan_get_stack_ids(&live[count], 1);
    if (live[count] == ptsan_default_id()) {
      ptsan_free_stack_ids(cursors[count]);
      break;
    }
    assert(live[count] != 0);
    assert(!owned[live[count]]);
    owned[live[count]] = 1;
    ++count;
  }
  assert(count > Recycled && count < 65536);
  count -= Recycled;
  // Force a return-cache drain for capacities through 4096 so the next batch
  // can claim some, but not all, of these IDs.
  for (unsigned i = 0; i != Recycled; ++i) {
    owned[live[count + i]] = 0;
  }
  ptsan_free_stack_ids(cursors[count]);

  const unsigned sizes[] = {2 * Recycled + 1, 65537};
  for (unsigned round = 0; round != 2; ++round) {
    unsigned n = sizes[round], valid = 0, defaults = 0;
    uintptr_t cursor = ptsan_stack_id_log.cursor;
    ptsan_get_stack_ids(mixed, n);
    for (unsigned i = 0; i != n; ++i) {
      if (mixed[i] == ptsan_default_id()) {
        ++defaults;
      } else {
        assert(mixed[i] != 0 && !owned[mixed[i]]);
        owned[mixed[i]] = 1;
        ++valid;
      }
    }
    assert(valid > 0 && defaults > 0);
    for (unsigned i = 0; i != n; ++i) {
      if (mixed[i] != ptsan_default_id()) {
        owned[mixed[i]] = 0;
      }
    }
    ptsan_free_stack_ids(cursor);
  }

  // Freeing mixed batches must make their unique IDs available again.
  ptsan_get_stack_ids(live + count, 32);
  for (unsigned i = 0; i != 32; ++i) {
    assert(live[count + i] && live[count + i] != ptsan_default_id());
  }
  count += 32;
  ptsan_free_stack_ids(start);
  return 0;
}
