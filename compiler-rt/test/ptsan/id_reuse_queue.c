//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Grow and turn over the recycled-ID pool with live batches. Reused IDs must
// remain unique; allocation order is not specified because thread caches may
// consume a batch in a different order from the shared queue.
// RUN: %clang_ptsan -O2 -mllvm -ptsan-id-reuse-distance=2 -mllvm -ptsan-instrument-globals=0 -mllvm -ptsan-stack-id-thread-buckets=0 %s -o %t
// RUN: %run %t
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-id-reuse-distance=2 -mllvm -ptsan-instrument-globals=0 -mllvm -ptsan-stack-id-thread-buckets=0 %s -o %t.id32
// RUN: %run %t.id32

#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stdlib.h>

enum { Count = 8192 };
static void *objects[Count];
static PTSanID first[Count], next[Count];

static int compare_ids(const void *a, const void *b) {
  PTSanID x = *(const PTSanID *)a, y = *(const PTSanID *)b;
  return (x > y) - (x < y);
}

static void allocate_batch(PTSanID *ids) {
  for (unsigned i = 0; i != Count; ++i) {
    objects[i] = malloc(1);
    assert(objects[i]);
    ids[i] = get_id(objects[i]);
  }
  qsort(ids, Count, sizeof(*ids), compare_ids);
  assert(ids[0] != 0);
  assert(ids[Count - 1] != ptsan_default_id());
  for (unsigned i = 1; i != Count; ++i) {
    assert(ids[i - 1] != ids[i]);
  }
}

static void release_batch(void) {
  for (unsigned i = 0; i != Count; ++i) {
    free(objects[i]);
  }
}

int main(void) {
  allocate_batch(first);
  release_batch();
  for (unsigned round = 0; round != 4; ++round) {
    allocate_batch(next);
    unsigned reused = 0;
    for (unsigned i = 0, j = 0; i != Count; ++i) {
      while (j != Count && first[j] < next[i]) {
        ++j;
      }
      reused += j != Count && first[j] == next[i];
    }
    assert(reused != 0);
    release_batch();
  }
  return 0;
}
