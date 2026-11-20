//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
#pragma once

#include "ptsan.h"
#include "ptsan_random.h"
#include "sanitizer_common/sanitizer_common.h"
#include "sanitizer_common/sanitizer_libc.h"

namespace __ptsan {

// Process-lifetime recycled-ID pool. Zero-initialize before use; callers
// provide locking. ID randomization selects and rotates 512-ID chunks, while
// the comparison mode retains the original FIFO behavior.
class RecycledIDQueue {
 public:
  static constexpr u32 RandomChunkSize = 512;

  u32 size() const { return Count + ReadyCount; }

  void initialize(uptr RequiredCapacity) {
    if (ClRandomizeIDs) {
      Random.initialize();
    }
    reserve(RequiredCapacity);
  }

  void reserve(uptr RequiredCapacity) {
    if (RequiredCapacity <= Capacity) {
      return;
    }

    uptr NewCapacity = RoundUpToPowerOfTwo(RequiredCapacity);
    auto *NewIDs = (PTSanID *)MmapOrDie(NewCapacity * sizeof(PTSanID),
                                      "PTSan recycled IDs");
    // A wrapped queue occupies two spans; copy them in FIFO order.
    if (Count) {
      u32 First = Min(Capacity - Head, (uptr)Count);
      internal_memcpy(NewIDs, IDs + Head, (uptr)First * sizeof(PTSanID));
      if (u32 Remaining = Count - First) {
        internal_memcpy(NewIDs + First, IDs,
                        (uptr)Remaining * sizeof(PTSanID));
      }
    }
    if (IDs) {
      UnmapOrDie(IDs, Capacity * sizeof(PTSanID));
    }
    IDs = NewIDs;
    Capacity = NewCapacity;
    Mask = (u32)NewCapacity - 1;
    Head = 0;
    Tail = Count;
  }

  void push(const PTSanID *Input, u32 N) {
    reserve((uptr)Count + N);
    u32 First = Min(Capacity - Tail, (uptr)N);
    internal_memcpy(IDs + Tail, Input, (uptr)First * sizeof(PTSanID));
    if (u32 Remaining = N - First) {
      internal_memcpy(IDs, Input + First, (uptr)Remaining * sizeof(PTSanID));
    }
    Tail = (Tail + N) & Mask;
    Count += N;
  }

  void pop(PTSanID *Output, u32 N) {
    CHECK(N <= size());
    if (ClRandomizeIDs) {
      popRandomized(Output, N);
      return;
    }
    popFIFO(Output, N);
  }

 private:
  void copyOut(u32 Offset, PTSanID *Output, u32 N) const {
    u32 Start = (Head + Offset) & Mask;
    u32 First = Min(Capacity - Start, (uptr)N);
    internal_memcpy(Output, IDs + Start, (uptr)First * sizeof(PTSanID));
    if (u32 Remaining = N - First) {
      internal_memcpy(Output + First, IDs,
                      (uptr)Remaining * sizeof(PTSanID));
    }
  }

  void copyWithin(u32 SourceOffset, u32 DestinationOffset, u32 N) {
    while (N) {
      u32 Source = (Head + SourceOffset) & Mask;
      u32 Destination = (Head + DestinationOffset) & Mask;
      u32 Part = Min(Min(Capacity - Source, Capacity - Destination), (uptr)N);
      internal_memcpy(IDs + Destination, IDs + Source,
                      (uptr)Part * sizeof(PTSanID));
      SourceOffset += Part;
      DestinationOffset += Part;
      N -= Part;
    }
  }

  void popFIFO(PTSanID *Output, u32 N) {
    CHECK(N <= Count);
    u32 First = Min(Capacity - Head, (uptr)N);
    internal_memcpy(Output, IDs + Head, (uptr)First * sizeof(PTSanID));
    if (u32 Remaining = N - First) {
      internal_memcpy(Output + First, IDs, (uptr)Remaining * sizeof(PTSanID));
    }
    Head = (Head + N) & Mask;
    Count -= N;
  }

  u32 popReady(PTSanID *Output, u32 Capacity) {
    u32 Taken = Min(Capacity, ReadyCount);
    internal_memcpy(Output, Ready + ReadyOffset,
                    (uptr)Taken * sizeof(PTSanID));
    ReadyOffset += Taken;
    ReadyCount -= Taken;
    return Taken;
  }

  void takeRandomChunk(PTSanID *Output) {
    u32 ChunkCount = Count / RandomChunkSize;
    CHECK_GT(ChunkCount, 0U);
    u32 Offset = Random.bounded(ChunkCount) * RandomChunkSize;
    u32 Rotation = Random.nextU32() & (RandomChunkSize - 1);
    u32 First = RandomChunkSize - Rotation;
    copyOut(Offset + Rotation, Output, First);
    copyOut(Offset, Output + First, Rotation);

    if (Offset) {
      copyWithin(0, Offset, RandomChunkSize);
    }
    Head = (Head + RandomChunkSize) & Mask;
    Count -= RandomChunkSize;
  }

  void popRandomized(PTSanID *Output, u32 N) {
    if (ReadyCount) {
      u32 Taken = popReady(Output, N);
      Output += Taken;
      N -= Taken;
    }

    while (N && Count >= RandomChunkSize) {
      if (N >= RandomChunkSize) {
        takeRandomChunk(Output);
        Output += RandomChunkSize;
        N -= RandomChunkSize;
        continue;
      }
      takeRandomChunk(Ready);
      ReadyOffset = 0;
      ReadyCount = RandomChunkSize;
      u32 Taken = popReady(Output, N);
      Output += Taken;
      N -= Taken;
    }

    if (N) {
      popFIFO(Output, N);
    }
  }

  PTSanID *IDs;
  uptr Capacity;
  u32 Mask;
  u32 Head;
  u32 Tail;
  u32 Count;
  PTSanID Ready[RandomChunkSize];
  u32 ReadyOffset;
  u32 ReadyCount;
  RandomStream Random;
};

} // namespace __ptsan
