//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Fresh chunks are selected without replacement and consumed cyclically from
// a random offset. Thread caches consume those IDs in reverse order. Turning
// randomization off restores ascending chunk and in-chunk allocation.
// RUN: %clang_ptsan -O2 -mllvm -ptsan-random-seed=1 -mllvm -ptsan-id-bucket-size=256 -mllvm -ptsan-instrument-globals=0 %s -o %t
// RUN: %run %t
// RUN: %clang_ptsan -O2 -mllvm -ptsan-random-seed=1 -mllvm -ptsan-id-bucket-size=256 -mllvm -ptsan-randomize-ids=0 -mllvm -ptsan-instrument-globals=0 -DSEQUENTIAL %s -o %t.sequential
// RUN: %run %t.sequential
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-random-seed=1 -mllvm -ptsan-id-bucket-size=256 -mllvm -ptsan-instrument-globals=0 %s -o %t.id32
// RUN: %run %t.id32
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-random-seed=1 -mllvm -ptsan-id-bucket-size=256 -mllvm -ptsan-randomize-ids=0 -mllvm -ptsan-instrument-globals=0 -DSEQUENTIAL %s -o %t.id32.sequential
// RUN: %run %t.id32.sequential

#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stdlib.h>

enum { Count = 8192, ChunkSize = 256 };
static void *Pointers[Count];
static PTSanID IDs[Count];

static int compare_ids(const void *A, const void *B) {
  PTSanID X = *(const PTSanID *)A, Y = *(const PTSanID *)B;
  return (X > Y) - (X < Y);
}

int main(void) {
  uint64_t IDSpaceBegin = ptsan_id_bits() == 32 ? UINT32_C(0x80000000) : 0;
  unsigned Adjacent = 0;
  unsigned Wraps = 0;
  int SequentialChunks = 1;
  for (unsigned I = 0; I != Count; ++I) {
    Pointers[I] = malloc(1);
    assert(Pointers[I]);
    IDs[I] = get_id(Pointers[I]);
    assert(IDs[I] != 0 && IDs[I] >= IDSpaceBegin);
    if (I) {
      uint64_t PreviousChunk = ((uint64_t)IDs[I - 1] - IDSpaceBegin) / ChunkSize;
      uint64_t Chunk = ((uint64_t)IDs[I] - IDSpaceBegin) / ChunkSize;
      if (Chunk == PreviousChunk) {
        uint64_t ChunkBegin = IDSpaceBegin + Chunk * ChunkSize;
        uint64_t ChunkEnd = ChunkBegin + ChunkSize;
        if ((uint64_t)IDs[I] + 1 != IDs[I - 1]) {
          uint64_t UsableBegin = ChunkBegin ? ChunkBegin : 1;
          uint64_t IDSpaceEnd = (uint64_t)ptsan_max_id() + 1;
          uint64_t UsableEnd = ChunkEnd < IDSpaceEnd ? ChunkEnd : IDSpaceEnd;
          assert((uint64_t)IDs[I - 1] == UsableBegin);
          assert((uint64_t)IDs[I] + 1 == UsableEnd);
          ++Wraps;
        }
        ++Adjacent;
      } else {
        SequentialChunks &= Chunk == PreviousChunk + 1;
      }
    }
  }
  assert(Adjacent > Count / 2);
#ifdef SEQUENTIAL
  assert(SequentialChunks);
  assert(Wraps == 0);
#else
  assert(!SequentialChunks);
  assert(Wraps != 0);
#endif
  qsort(IDs, Count, sizeof(*IDs), compare_ids);
  for (unsigned I = 1; I != Count; ++I)
    assert(IDs[I - 1] != IDs[I]);
  for (unsigned I = 0; I != Count; ++I)
    free(Pointers[I]);
  return 0;
}
