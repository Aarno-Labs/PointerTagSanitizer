//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
#pragma once

#include "ptsan.h"

namespace __ptsan {

constexpr u32 ChaChaBlockWords = 16;
constexpr u32 RandomBatchBlocks = 16;

class RandomStream {
public:
  void initialize();
  ALWAYS_INLINE u32 nextU32() {
    // Keep the validated position local: a signal may exhaust or refill the
    // shared buffer before consumption. Repeated/skipped draws are acceptable.
    u32 I = __atomic_load_n(&Index, __ATOMIC_RELAXED);
    if (UNLIKELY(I >= BufferWords)) {
      refill();
      I = 0;
    }
    __atomic_store_n(&Index, I + 1, __ATOMIC_RELAXED);
    return Buffer[I];
  }
  u32 bounded(u32 Bound);

private:
  static constexpr u32 BufferWords = ChaChaBlockWords * RandomBatchBlocks;
  // Keep batched block generation off the ordinary draw path.
  NOINLINE void refill();

  u32 Key[8];
  alignas(8) u64 Counter;
  u32 Buffer[BufferWords];
  u32 Index;
  static_assert(__atomic_always_lock_free(sizeof(Index), nullptr));
};

} // namespace __ptsan
