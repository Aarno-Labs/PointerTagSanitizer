//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict %s -o %t
// RUN: %t posix
// RUN: %t aligned
// RUN: not %t posix oob 2>&1 | FileCheck %s
// RUN: not %t aligned oob 2>&1 | FileCheck %s
// CHECK: Found bad POINT access
//
// ID32 origins must preserve alignment without weakening allocation bounds.
// RUN: %ptsan_musl_id32_strict %s -o %t.id32
// RUN: %t.id32 posix
// RUN: %t.id32 aligned
// RUN: not %t.id32 posix oob 2>&1 | FileCheck %s
// RUN: not %t.id32 aligned oob 2>&1 | FileCheck %s

// Check the decoded address, not the packed pointer's low bits. In 32-bit-ID
// musl builds, aligned_alloc.c needs both nochecks and strip-ptrtoint: the
// allocator inspects raw alignment and locally reconstructs a metadata-page
// pointer. Its wrappers must still return a bounded, tagged allocation.

#include "../Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stdlib.h>

__attribute__((noinline)) static void read_past_end(char *p, size_t size) {
  volatile char value = p[size];
  (void)value;
}

int main(int argc, char **argv) {
  assert(argc == 2 || argc == 3);
  assert(argv[1][0] == 'p' || argv[1][0] == 'a');
  const size_t alignments[] = {64, 4096, 65536};
  for (size_t i = 0; i < sizeof(alignments) / sizeof(alignments[0]); ++i) {
    size_t alignment = alignments[i];
    size_t size = 2 * alignment;
    for (int repetition = 0; repetition < 16; ++repetition) {
      void *allocation = NULL;
      if (argv[1][0] == 'p') {
        assert(posix_memalign(&allocation, alignment, size) == 0);
      } else {
        allocation = aligned_alloc(alignment, size);
      }
      assert(allocation != NULL);
      assert((ptsan_get_address(allocation) & (alignment - 1)) == 0);
      assert((ptsan_get_payload(allocation) & (alignment - 1)) == 0);
      uint32_t id = get_id(allocation);
      assert(id != 0);
      assert(ptsan_get_size(id) == size);
      char *p = allocation;
      p[0] = 17;
      p[size - 1] = 29;
      assert(p[0] == 17 && p[size - 1] == 29);
      if (argc == 3) {
        read_past_end(p, size);
      }
      free(allocation);
    }
  }
  return 0;
}
