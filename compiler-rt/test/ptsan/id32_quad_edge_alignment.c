//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-random-seed=1 -o %t %s
// RUN: %run %t

// Olden/voronoi rotates among four 32-byte edges using the low seven address
// bits. Musl's memalign and posix_memalign share aligned_alloc, which also uses
// integer address arithmetic.
// Check the decoded address explicitly: a packed alignment test alone can
// pass even when that arithmetic has returned a misaligned raw address.
#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

struct Edge {
  struct Edge *next;
  void *data;
  long seen;
  long padding;
};

__attribute__((noinline)) static struct Edge *rotate(struct Edge *p) {
  uintptr_t bits = (uintptr_t)p;
  return (struct Edge *)(((bits + 32) & 127) | (bits & ~(uintptr_t)127));
}

int main(void) {
  assert(sizeof(struct Edge) == 32);
  for (unsigned allocation = 0; allocation < 16; ++allocation) {
    void *allocation_base = NULL;
    assert(posix_memalign(&allocation_base, 128, 128) == 0);
    struct Edge *edges = allocation_base;
    assert(edges != NULL);
    uint64_t raw = ptsan_get_address(edges);
    uint32_t id = get_id(edges);
    assert(id != 0 && id != ptsan_default_id());
    assert((raw & 127) == 0);
    assert((ptsan_get_payload(edges) & 127) == 0);

    struct Edge *p = edges;
    for (unsigned i = 0; i < 4; ++i) {
      assert(get_id(p) == id);
      assert(ptsan_get_address(p) == raw + i * sizeof(struct Edge));
      p->seen = i + 1;
      p = rotate(p);
    }
    assert(p == edges);
    for (unsigned i = 0; i < 4; ++i) {
      assert(edges[i].seen == i + 1);
    }
    free(edges);
  }
}
