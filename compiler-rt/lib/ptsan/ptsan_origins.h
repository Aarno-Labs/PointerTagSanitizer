//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
#pragma once

#include "ptsan.h"
#include "ptsan_constants.h"

#ifdef PTSAN_32_BIT_IDS
#include "ptsan_random.h"

extern "C" {

SANITIZER_INTERFACE_ATTRIBUTE
extern u32 *ptsan_origins;

SANITIZER_INTERFACE_ATTRIBUTE
u32 ptsan_choose_stack_origin(u64 alignment);

// The inline fast-ID path consumes cached salts and refills at the sentinel.
// Refills return a separate salt and leave the entire cache for later frames.
// Slow-ID paths, including wrapped handlers, use ptsan_choose_stack_origin.
SANITIZER_INTERFACE_ATTRIBUTE extern THREADLOCAL u32 *ptsan_stack_origin_next;
SANITIZER_INTERFACE_ATTRIBUTE u32 ptsan_refill_stack_origins();
}

namespace __ptsan {

void initializeOrigins();

// The slow selector keeps TLS access out of pre-TLS global registration.
NOINLINE RandomStream &origin_random();

ALWAYS_INLINE RandomStream &origin_random(RandomStream *Known) {
  // A thread record may exist before runtime initialization finishes.
  return Known && LIKELY(ptsan_initialized) ? *Known : origin_random();
}

// Returns the random aligned component, not the encoded allocation start.
ALWAYS_INLINE u32 choose_id32_origin(u64 Alignment,
                                    RandomStream *Known = nullptr) {
  RandomStream &Random = origin_random(Known);
  constexpr u64 SaltSpace = 1ULL << 31;
  CHECK(IsPowerOfTwo(Alignment));
  Alignment = Max(Alignment, (u64)ClID32OriginQuantum);
  // A nonzero aligned salt must fit below 2 GiB.
  CHECK_LT(Alignment, SaltSpace);
  u32 Salt;
  do {
    Salt = Random.nextU32() & (SaltSpace - 1) & ~(Alignment - 1);
  } while (Salt == 0);
  // A nonzero salt leaves at least one quantum of underflow room.
  return Salt;
}

// Combines the salt with the raw base's page/alignment remainder.
ALWAYS_INLINE u32 choose_id32_payload_start(u64 Base, u64 Alignment,
                                         RandomStream *Known = nullptr) {
  u64 Quantum = Max(Alignment, (u64)ClID32OriginQuantum);
  return choose_id32_origin(Quantum, Known) + (Base & (Quantum - 1));
}

} // namespace __ptsan
#endif
