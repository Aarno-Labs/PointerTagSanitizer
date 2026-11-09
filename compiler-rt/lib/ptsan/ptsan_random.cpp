//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
#include "ptsan_random.h"

namespace __ptsan {
namespace {

using ChaChaRow = u32 __attribute__((vector_size(64)));

static ALWAYS_INLINE void quarterRound(ChaChaRow &A, ChaChaRow &B, ChaChaRow &C,
                                       ChaChaRow &D) {
  A += B;
  D ^= A;
  D = (D << 16) | (D >> 16);
  C += D;
  B ^= C;
  B = (B << 12) | (B >> 20);
  A += B;
  D ^= A;
  D = (D << 8) | (D >> 24);
  C += D;
  B ^= C;
  B = (B << 7) | (B >> 25);
}

// Rotate words within each independent 128-bit ChaCha row.
template <unsigned Shift>
static ALWAYS_INLINE void rotateLanes(ChaChaRow &Value) {
  constexpr unsigned A = Shift, B = (Shift + 1) % 4;
  constexpr unsigned C = (Shift + 2) % 4, D = (Shift + 3) % 4;
  Value = __builtin_shufflevector(Value, Value, A, B, C, D, A + 4, B + 4, C + 4,
                                  D + 4, A + 8, B + 8, C + 8, D + 8, A + 12,
                                  B + 12, C + 12, D + 12);
}

// Four independent states each carry four blocks in separate 128-bit lanes.
// LLVM lowers the generic vectors to the instructions available on each target.
// Every block remains the ChaCha12 block at its assigned counter.
static ALWAYS_INLINE void generateBlocks(const u32 *Key, u64 Counter,
                                         u32 *Output) {
  constexpr unsigned Words = sizeof(ChaChaRow) / sizeof(u32);
  constexpr unsigned BlocksPerVector = Words / 4;
  static_assert(4 * BlocksPerVector == RandomBatchBlocks);
  constexpr u32 Constants[] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
  ChaChaRow SeedA, SeedB, SeedC;
#pragma unroll
  for (unsigned I = 0; I != Words; ++I) {
    SeedA[I] = Constants[I % 4];
    SeedB[I] = Key[I % 4];
    SeedC[I] = Key[4 + I % 4];
  }
  ChaChaRow A[4], B[4], C[4], D[4], SeedD[4];
#pragma unroll
  for (unsigned G = 0; G != 4; ++G) {
    A[G] = SeedA;
    B[G] = SeedB;
    C[G] = SeedC;
#pragma unroll
    for (unsigned I = 0; I != Words; ++I) {
      u64 Block = Counter + G * BlocksPerVector + I / 4;
      SeedD[G][I] = I % 4 == 0   ? (u32)Block
                    : I % 4 == 1 ? (u32)(Block >> 32)
                                 : 0;
    }
    D[G] = SeedD[G];
  }
  for (unsigned Round = 0; Round != 6; ++Round) {
#pragma unroll
    for (unsigned G = 0; G != 4; ++G) {
      quarterRound(A[G], B[G], C[G], D[G]);
      rotateLanes<1>(B[G]);
      rotateLanes<2>(C[G]);
      rotateLanes<3>(D[G]);
      quarterRound(A[G], B[G], C[G], D[G]);
      rotateLanes<3>(B[G]);
      rotateLanes<2>(C[G]);
      rotateLanes<1>(D[G]);
    }
  }
#pragma unroll
  for (unsigned G = 0; G != 4; ++G) {
    A[G] += SeedA;
    B[G] += SeedB;
    C[G] += SeedC;
    D[G] += SeedD[G];
#pragma unroll
    for (unsigned I = 0; I != Words; ++I) {
      unsigned Base = (G * BlocksPerVector + I / 4) * ChaChaBlockWords + I % 4;
      Output[Base] = A[G][I];
      Output[Base + 4] = B[G][I];
      Output[Base + 8] = C[G][I];
      Output[Base + 12] = D[G][I];
    }
  }
}

#if defined(__x86_64__)
__attribute__((target_clones("avx512f", "avx2", "default")))
#endif
static void
generateBatch(const u32 *Key, u64 Counter, u32 *Output) {
  generateBlocks(Key, Counter, Output);
}

} // namespace

static u64 splitmix64(u64 &State) {
  u64 Value = (State += 0x9e3779b97f4a7c15ULL);
  Value = (Value ^ (Value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  Value = (Value ^ (Value >> 27)) * 0x94d049bb133111ebULL;
  return Value ^ (Value >> 31);
}

void RandomStream::initialize() {
  if (ClRandomSeed) {
    u64 State = ClRandomSeed;
    for (u32 I = 0; I != 4; ++I) {
      u64 Value = splitmix64(State);
      Key[I * 2] = (u32)Value;
      Key[I * 2 + 1] = (u32)(Value >> 32);
    }
  } else if (!GetRandom(Key, sizeof(Key))) {
    Printf("PTSan could not initialize its random generator\n");
    internal__exit(1);
  }
  Counter = 0;
  __atomic_store_n(&Index, BufferWords, __ATOMIC_RELAXED);
}

void RandomStream::refill() {
  // Reserve disjoint counter ranges even if a signal interrupts a refill.
  u64 First = __atomic_fetch_add(&Counter, RandomBatchBlocks, __ATOMIC_RELAXED);
  // The kernels keep round state local and store only completed output words.
  // As before, signal interruption may duplicate or skip cached draws.
  generateBatch(Key, First, Buffer);
}

u32 RandomStream::bounded(u32 Bound) { return nextU32() % Bound; }

} // namespace __ptsan
