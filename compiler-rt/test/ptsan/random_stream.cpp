//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clangxx_raw -std=c++17 -O2 -I%S/../../lib %s -o %t
// RUN: %run %t
//
// Compile the real generator without instrumentation. Check every supported
// target against a scalar ChaCha12 reference, including counter carries and
// unaligned output. Target-specific wrappers exercise the same kernel that
// target_clones inlines; buffered streams exercise the generated dispatcher.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// This standalone test provides the freestanding helper using the host libc.
#define SANITIZER_COMMON_NO_REDEFINE_BUILTINS
#include "ptsan/ptsan_random.cpp"

unsigned ClRandomSeed = 1;
namespace __sanitizer {
bool GetRandom(void *, uptr, bool) { abort(); }
void Printf(const char *, ...) { abort(); }
void internal__exit(int Code) { exit(Code); }
} // namespace __sanitizer

static u32 rotate(u32 V, unsigned N) { return (V << N) | (V >> (32 - N)); }
static void quarter(u32 *X, unsigned A, unsigned B, unsigned C, unsigned D) {
  X[A] += X[B];
  X[D] = rotate(X[D] ^ X[A], 16);
  X[C] += X[D];
  X[B] = rotate(X[B] ^ X[C], 12);
  X[A] += X[B];
  X[D] = rotate(X[D] ^ X[A], 8);
  X[C] += X[D];
  X[B] = rotate(X[B] ^ X[C], 7);
}
static void reference(const u32 *Key, u64 Counter, u32 *Out) {
  u32 Initial[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
  for (unsigned I = 0; I < 8; ++I)
    Initial[I + 4] = Key[I];
  Initial[12] = Counter;
  Initial[13] = Counter >> 32;
  memcpy(Out, Initial, sizeof(Initial));
  for (unsigned I = 0; I < 6; ++I) {
    quarter(Out, 0, 4, 8, 12);
    quarter(Out, 1, 5, 9, 13);
    quarter(Out, 2, 6, 10, 14);
    quarter(Out, 3, 7, 11, 15);
    quarter(Out, 0, 5, 10, 15);
    quarter(Out, 1, 6, 11, 12);
    quarter(Out, 2, 7, 8, 13);
    quarter(Out, 3, 4, 9, 14);
  }
  for (unsigned I = 0; I < 16; ++I)
    Out[I] += Initial[I];
}

using Generate = void (*)(const u32 *, u64, u32 *);

static void generateDefault(const u32 *Key, u64 Counter, u32 *Output) {
  __ptsan::generateBlocks(Key, Counter, Output);
}

#if defined(__x86_64__)
__attribute__((target("avx2"))) static void
generateAVX2(const u32 *Key, u64 Counter, u32 *Output) {
  __ptsan::generateBlocks(Key, Counter, Output);
}

__attribute__((target("avx512f"))) static void
generateAVX512(const u32 *Key, u64 Counter, u32 *Output) {
  __ptsan::generateBlocks(Key, Counter, Output);
}
#endif

static void checkBatch(const u32 *Key, u64 Counter, Generate Kernel) {
  alignas(64) u32 Storage[258];
  Storage[0] = Storage[257] = 0x12345678;
  Kernel(Key, Counter, Storage + 1);
  assert(Storage[0] == 0x12345678 && Storage[257] == 0x12345678);
  for (unsigned Block = 0; Block < 16; ++Block) {
    u32 Expected[16];
    reference(Key, Counter + Block, Expected);
    assert(memcmp(Storage + 1 + Block * 16, Expected, sizeof(Expected)) == 0);
  }
}

int main() {
  Generate Kernels[4] = {generateDefault, __ptsan::generateBatch};
  unsigned KernelCount = 2;
#if defined(__x86_64__)
  __builtin_cpu_init();
  if (__builtin_cpu_supports("avx2"))
    Kernels[KernelCount++] = generateAVX2;
  if (__builtin_cpu_supports("avx512f"))
    Kernels[KernelCount++] = generateAVX512;
#endif
  u64 Counters[] = {0,
                    1,
                    15,
                    0xfffffff8ULL,
                    0xffffffffULL,
                    0x100000000ULL,
                    0x12345678abcdef01ULL,
                    ~0ULL - 15,
                    ~0ULL};
  u32 Key[8];
  u32 State = 0x12345678;
  for (unsigned Trial = 0; Trial < 256; ++Trial) {
    for (u32 &Word : Key) {
      State = State * 1664525 + 1013904223;
      Word = Trial == 0 ? 0 : Trial == 1 ? ~0U : State;
    }
    for (u64 Counter : Counters) {
      for (unsigned I = 0; I < KernelCount; ++I)
        checkBatch(Key, Counter, Kernels[I]);
    }
  }
  // Crossing many refill boundaries must preserve the complete public stream.
  // A fixed seed is test-only; production still obtains its 256-bit key from
  // OS.
  const unsigned Seeds[] = {1U, 2U, 12345U, ~0U};
  for (unsigned Seed : Seeds) {
    ClRandomSeed = Seed;
    u64 SeedState = Seed;
    for (unsigned I = 0; I < 4; ++I) {
      u64 V = (SeedState += 0x9e3779b97f4a7c15ULL);
      V = (V ^ (V >> 30)) * 0xbf58476d1ce4e5b9ULL;
      V = (V ^ (V >> 27)) * 0x94d049bb133111ebULL;
      V ^= V >> 31;
      Key[I * 2] = V;
      Key[I * 2 + 1] = V >> 32;
    }
    __ptsan::RandomStream Stream;
    Stream.initialize();
    for (unsigned Block = 0; Block < 1024; ++Block) {
      u32 Expected[16];
      reference(Key, Block, Expected);
      for (u32 Word : Expected)
        assert(Stream.nextU32() == Word);
    }
  }
  puts("ChaCha12 targets and buffered streams match the scalar reference");
}
