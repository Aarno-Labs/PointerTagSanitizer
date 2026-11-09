//===-- ptsan_id_cache.h - Batched runtime IDs ---------------------*- C++ -*-===//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "ptsan.h"

namespace __ptsan {

// Usable IDs in an aligned bucket, excluding reserved endpoint IDs.
// The exclusive end may exceed the PTSanID range.
struct IDBucket {
  PTSanID start;
  u64 end;
};

// Initialize the shared bucket allocator and recycled-ID queue before use.
void initializeIDPool(u32 bucket_size);

// Startup-only stack reservations, with signals blocked and before any ID
// allocations. CLI validation ensures the reservation count fits the pool.
IDBucket claimIDBucket();

// Initialization callers can allocate without a thread/cache. Return zero if
// exhausted. Instrumented musl blocks signals; all configurations lock the
// shared pool internally when multithreaded.
PTSanID takeIDDirect();

// Return a real ID whose metadata has been cleared, without a thread/cache.
// Signal masking and locking follow the same rules as takeIDDirect().
void returnIDDirect(PTSanID id);

static constexpr u32 NormalIDCacheCapacity = 4096;
#ifdef PTSAN_32_BIT_IDS
// ID32 can retain larger per-thread batches without pressuring its ID space.
static constexpr u32 MultithreadedIDCacheCapacity = NormalIDCacheCapacity;
#else
static constexpr u32 MultithreadedIDCacheCapacity = 512;
#endif
static constexpr u32 SignalIDCacheCapacity = 64;

// Must be valid when zero-initialized by mmap; release is explicit, not a
// destructor. This class only moves unused IDs, never their bounds metadata.
class IDCache {
 public:
  void initialize(u32 active_capacity = NormalIDCacheCapacity);
  void setActiveCapacity(u32 active_capacity);
  PTSanID take();             // Return zero if exhausted.
  void returnID(PTSanID id);  // Accept a real ID whose metadata is already cleared.
  void release();         // Return all cached IDs to the shared pool.

 private:
  struct AllocationCache {
    PTSanID IDs[NormalIDCacheCapacity];
    u32 Count;
  } Allocations;

  struct ReturnCache {
    PTSanID IDs[NormalIDCacheCapacity];
    u32 Count;
  } Returns;

  // Number of usable slots in both caches. Allocation refills are capped at
  // this value, and the packed return buffer drains when it reaches it.
  u32 ActiveCapacity;

  PTSanID refill();
  void releaseUnusedIDs();
  void drainReturns();
};

// A distinct per-thread instance used only by instrumented-musl signal
// handlers. Atomic heads and sparse return slots preserve ownership across
// nested handlers and nonlocal exits.
class SignalSafeIDCache {
 public:
  PTSanID take();
  void returnID(PTSanID id);
  void release();

 private:
  static constexpr u32 CountBits = 16;

  // A new generation invalidates readers interrupted by refill. Generation
  // wraps, but no read will realistically span 2^48 refills before completing.
  struct alignas(8) AllocationHead {
    u64 Count : CountBits;
    u64 Generation : 64 - CountBits;
  };

  struct AllocationCache {
    PTSanID IDs[SignalIDCacheCapacity];
    AllocationHead Head;
  } Allocations;

  struct ReturnCache {
    PTSanID IDs[SignalIDCacheCapacity];
    // A hint; atomic slot ownership determines whether insertion succeeds.
    u32 Next;
  } Returns;

  PTSanID takeCached();
  PTSanID refill();
  bool returnCached(PTSanID id);
  void releaseUnusedIDs();
  void drainReturns();

  static_assert(sizeof(AllocationHead) == sizeof(u64));
  static_assert(alignof(AllocationHead) == alignof(u64));
  static_assert(__atomic_always_lock_free(sizeof(AllocationHead), nullptr));
  static_assert(__atomic_always_lock_free(sizeof(PTSanID), nullptr));
  static_assert(SignalIDCacheCapacity < ((u64)1 << CountBits));
};

} // namespace __ptsan
