//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Recycled IDs are selected and rotated in bounded chunks. Disabling ID
// randomization retains the old FIFO order through the shared pool.
// RUN: %clang_ptsan -O2 -mllvm -ptsan-random-seed=1 -mllvm -ptsan-id-reuse-distance=0 -mllvm -ptsan-instrument-globals=0 -mllvm -ptsan-stack-id-thread-buckets=0 %s -o %t
// RUN: %run %t
// RUN: %clang_ptsan -O2 -mllvm -ptsan-random-seed=1 -mllvm -ptsan-randomize-ids=0 -mllvm -ptsan-id-reuse-distance=0 -mllvm -ptsan-instrument-globals=0 -mllvm -ptsan-stack-id-thread-buckets=0 -DSEQUENTIAL %s -o %t.sequential
// RUN: %run %t.sequential
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-random-seed=1 -mllvm -ptsan-id-reuse-distance=0 -mllvm -ptsan-instrument-globals=0 -mllvm -ptsan-stack-id-thread-buckets=0 %s -o %t.id32
// RUN: %run %t.id32
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-random-seed=1 -mllvm -ptsan-randomize-ids=0 -mllvm -ptsan-id-reuse-distance=0 -mllvm -ptsan-instrument-globals=0 -mllvm -ptsan-stack-id-thread-buckets=0 -DSEQUENTIAL %s -o %t.id32.sequential
// RUN: %run %t.id32.sequential

#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stdlib.h>

enum { CacheSize = 4096, Count = 2 * CacheSize };

int main(void) {
  void *Pointers[Count];
  PTSanID IDs[Count];
  for (unsigned I = 0; I != Count; ++I) {
    Pointers[I] = malloc(1);
    assert(Pointers[I]);
    IDs[I] = get_id(Pointers[I]);
  }
  for (unsigned I = 0; I != Count; ++I) {
    free(Pointers[I]);
  }

  // A smaller fresh chunk can leave an unfinished allocation-cache batch.
  // Drain those never-used IDs before checking the first recycled ID.
  void *Fresh[CacheSize];
  unsigned FreshCount = 0;
  void *Reused;
  unsigned ReusedIndex;
  for (;;) {
    Reused = malloc(1);
    assert(Reused);
    PTSanID ID = get_id(Reused);
    for (ReusedIndex = 0; ReusedIndex != Count; ++ReusedIndex)
      if (IDs[ReusedIndex] == ID)
        break;
    if (ReusedIndex != Count)
      break;
    assert(FreshCount < CacheSize);
    Fresh[FreshCount++] = Reused;
  }
#ifdef SEQUENTIAL
  assert(ReusedIndex == CacheSize - 1);
#else
  assert(ReusedIndex != CacheSize - 1);
#endif
  free(Reused);
  for (unsigned I = 0; I != FreshCount; ++I)
    free(Fresh[I]);
  return 0;
}
