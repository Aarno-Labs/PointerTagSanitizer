//===-- ptsan_id_cache.cpp - Batched runtime IDs ----------------------------===//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ptsan_id_cache.h"
#include "ptsan_constants.h"
#include "ptsan_id_queue.h"
#include "ptsan_random.h"
#include "ptsan_signal.h"
#include "ptsan_thread.h"
#include "sanitizer_common/sanitizer_mutex.h"

namespace __ptsan {

// Shared process-wide pool of IDs not currently assigned to an object.
// Instrumented-musl operations block signals, and all configurations hold the
// pool lock once multiple threads exist.
// Randomize bucket placement and the starting ID within each bucket. Globals
// and thread caches draw from the same fresh pool.
class IDPool {
public:
  void initialize(u32 Size) {
    CHECK(Size && IsPowerOfTwo(Size));
    BucketSize = Size;
    BucketCount = (NumIDs - PTSAN_ID_SPACE_BEGIN) / BucketSize;
    if (!BucketCount) {
      Printf("PTSan ID bucket size exceeds usable ID space\n");
      internal__exit(1);
    }
    ClaimedBuckets = (u8 *)MmapNoReserveOrDie(
        ((uptr)BucketCount + 7) / 8, "PTSan claimed ID buckets");
    RemainingBuckets = BucketCount;
    NextSequentialBucket = 0;
    FreshIDBegin = 0;
    FreshIDEnd = 0;
    NextFreshID = 0;
    FreshIDsRemaining = 0;
    if (ClRandomizeIDs) {
      BucketRandom.initialize();
    }
    RecycledIDs.initialize((uptr)ClIDReuseDistance + 1);
    HasIDs = true;
  }

  IDBucket claimBucket() {
    CHECK_GT(RemainingBuckets, 0U);
    u32 Bucket;
    if (ClRandomizeIDs) {
      do {
        Bucket = BucketRandom.bounded(BucketCount);
      } while (isClaimed(Bucket));
    } else {
      while (isClaimed(NextSequentialBucket)) {
        ++NextSequentialBucket;
      }
      Bucket = NextSequentialBucket++;
    }
    ClaimedBuckets[Bucket / 8] |= 1U << (Bucket % 8);
    --RemainingBuckets;
    u64 Start = PTSAN_ID_SPACE_BEGIN + (u64)Bucket * BucketSize;
    // Reserve zero and the ID16 fallback slot when opening their buckets.
    // Every other ID remains available, including the namespace's final bucket.
    return {(PTSanID)Max(Start, u64{1}),
            Min(Start + BucketSize, (u64)LastIDNo + 1)};
  }

  // Signals must be blocked by the caller. Prefer a recycled candidate pool
  // of the configured size, then fresh IDs; relax that target only when the
  // fresh namespace is exhausted.
  u32 take(PTSanID *IDs, u32 Capacity) {
    bool Locked = lock();
    u32 Count;
    if ((s64)RecycledIDs.size() - Capacity >= ClIDReuseDistance) {
      Count = Capacity;
      RecycledIDs.pop(IDs, Count);
    } else if (FreshIDsRemaining || RemainingBuckets) {
      Count = takeFreshIDs(IDs, Capacity);
    } else {
      Count = Min(Capacity, RecycledIDs.size());
      if (Count) {
        RecycledIDs.pop(IDs, Count);
      }
    }
    unlock(Locked);
    return Count;
  }

  // Signals must be blocked by the caller, including while the queue grows.
  void returnIDs(const PTSanID *IDs, u32 Count) {
    if (!Count) {
      return;
    }
    bool Locked = lock();
    RecycledIDs.push(IDs, Count);
    unlock(Locked);
  }

  // Best-effort availability hint, not an exact emptiness test. Stale results
  // in either direction are acceptable: false may report exhaustion early,
  // while true still requires taking IDs through the pool's normal locking.
  // No ID ownership or metadata is published through this flag.
  bool hasIDs() const { return HasIDs; }

private:
  bool isClaimed(u32 Bucket) const {
    return ClaimedBuckets[Bucket / 8] & (1U << (Bucket % 8));
  }

  u32 takeFreshIDs(PTSanID *IDs, u32 Capacity) {
    if (!FreshIDsRemaining) {
      if (!RemainingBuckets) {
        return 0;
      }
      IDBucket Bucket = claimBucket();
      FreshIDBegin = Bucket.start;
      FreshIDEnd = Bucket.end;
      // Endpoint buckets can exclude zero or the ID16 fallback slot.
      FreshIDsRemaining = (u32)(FreshIDEnd - FreshIDBegin);
      NextFreshID = FreshIDBegin;
      if (ClRandomizeIDs) {
        NextFreshID += BucketRandom.bounded(FreshIDsRemaining);
      }
    }
    u32 Count = Min(Capacity, FreshIDsRemaining);
    for (u32 I = 0; I < Count; ++I) {
      IDs[I] = (PTSanID)NextFreshID++;
      if (NextFreshID == FreshIDEnd) {
        NextFreshID = FreshIDBegin;
      }
    }
    FreshIDsRemaining -= Count;
    return Count;
  }

  bool lock() {
    if (multithreaded) {
      Lock.Lock();
      return true;
    }
    return false;
  }

  void unlock(bool Locked) {
    HasIDs = FreshIDsRemaining != 0 || RemainingBuckets != 0 ||
             RecycledIDs.size() != 0;
    if (Locked) {
      Lock.Unlock();
    }
  }

  StaticSpinMutex Lock;
  RecycledIDQueue RecycledIDs;
  // Start of the chunk
  u64 FreshIDBegin;
  // Cursor to next id in the chunk
  u64 NextFreshID;
  // End of the chunk
  u64 FreshIDEnd;
  // How many IDs left in the chunk
  u32 FreshIDsRemaining;
  // Bitmap of claimed buckets. Once a bucket is claimed
  // here, it's never freed
  u8 *ClaimedBuckets;
  u32 BucketCount;
  u32 BucketSize;
  u32 RemainingBuckets;
  u32 NextSequentialBucket;
  RandomStream BucketRandom;
  bool HasIDs;
};

static IDPool Pool;

void initializeIDPool(u32 BucketSize) {
  Pool.initialize(BucketSize);
}

IDBucket claimIDBucket() {
  return Pool.claimBucket();
}

PTSanID takeIDDirect() {
  ScopedSignalBlock block;
  PTSanID id = 0;
  Pool.take(&id, 1);
  return id;
}

void returnIDDirect(PTSanID id) {
  ScopedSignalBlock block;
  Pool.returnIDs(&id, 1);
}

void IDCache::initialize(u32 active_capacity) {
  setActiveCapacity(active_capacity);
}

void IDCache::setActiveCapacity(u32 active_capacity) {
  CHECK_GT(active_capacity, 0U);
  CHECK_LE(active_capacity, NormalIDCacheCapacity);
  CHECK_EQ(Allocations.Count, 0U);
  CHECK_EQ(Returns.Count, 0U);
  ActiveCapacity = active_capacity;
}

PTSanID IDCache::take() {
  if (Allocations.Count) {
    return Allocations.IDs[--Allocations.Count];
  }
  if (!Pool.hasIDs()) {
    // Do not report exhaustion while this thread is quarantining returned
    // IDs. Publish them only under pressure, preserving the normal batching
    // delay without needlessly falling back to the default ID.
    if (!Returns.Count) {
      return 0;
    }
    drainReturns();
  }
  return refill();
}

// Refill an empty cache, then take one ID. Return zero if the pool is empty.
PTSanID IDCache::refill() {
  ScopedSignalBlock block;
  u32 count = Pool.take(Allocations.IDs, ActiveCapacity);
  // Pool.hasIDs() is only a hint. Another thread or a signal handler can
  // consume the final shared IDs before take acquires the lock.
  if (!count) {
    return 0;
  }
  Allocations.Count = count - 1;
  return Allocations.IDs[count - 1];
}

void IDCache::releaseUnusedIDs() {
  ScopedSignalBlock block;
  Pool.returnIDs(Allocations.IDs, Allocations.Count);
  Allocations.Count = 0;
}

void IDCache::drainReturns() {
  ScopedSignalBlock block;
  Pool.returnIDs(Returns.IDs, Returns.Count);
  Returns.Count = 0;
}

void IDCache::returnID(PTSanID id) {
  if (Returns.Count == ActiveCapacity) {
    drainReturns();
  }
  Returns.IDs[Returns.Count++] = id;
}

void IDCache::release() {
  releaseUnusedIDs();
  drainReturns();
}

PTSanID SignalSafeIDCache::take() {
  if (PTSanID id = takeCached()) {
    return id;
  }
  if (!Pool.hasIDs()) {
    // Sparse return slots have no reliable count. Scan and publish them under
    // pressure; refill verifies whether that made an ID available.
    drainReturns();
  }
  return Pool.hasIDs() ? refill() : 0;
}

PTSanID SignalSafeIDCache::takeCached() {
  AllocationHead head;
  __atomic_load(&Allocations.Head, &head, __ATOMIC_ACQUIRE);
  while (head.Count) {
    PTSanID id = Allocations.IDs[head.Count - 1];
    auto desired = head;
    desired.Count--;
    if (__atomic_compare_exchange(&Allocations.Head, &head, &desired, false,
                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      return id;
    }
    // Failure refreshes head. Retry with its count and reread the slot.
  }
  return 0;
}

PTSanID SignalSafeIDCache::refill() {
  ScopedSignalBlock block;
  // A nested handler may have refilled the cache before signals were blocked.
  if (Allocations.Head.Count) {
    return Allocations.IDs[--Allocations.Head.Count];
  }

  u32 count = Pool.take(Allocations.IDs, SignalIDCacheCapacity);
  if (!count) {
    return 0;
  }
  Allocations.Head.Generation++;
  Allocations.Head.Count = count - 1;
  return Allocations.IDs[count - 1];
}

bool SignalSafeIDCache::returnCached(PTSanID id) {
  u32 slot = Returns.Next;
  PTSanID empty = 0;
  if (!__atomic_compare_exchange_n(&Returns.IDs[slot], &empty, id, false,
                                   __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
    return false;
  }
  // Next is only a hint. An interruption can leave it stale and cause an early
  // drain, but the CAS above decides slot ownership.
  Returns.Next = (slot + 1) % SignalIDCacheCapacity;
  return true;
}

void SignalSafeIDCache::returnID(PTSanID id) {
  while (!returnCached(id)) {
    drainReturns();
  }
}

void SignalSafeIDCache::releaseUnusedIDs() {
  ScopedSignalBlock block;
  AllocationHead head = Allocations.Head;
  Pool.returnIDs(Allocations.IDs, head.Count);
  head.Generation++;
  head.Count = 0;
  Allocations.Head = head;
}

void SignalSafeIDCache::drainReturns() {
  ScopedSignalBlock block;
  PTSanID IDs[SignalIDCacheCapacity];
  u32 Count = 0;
  for (u32 slot = 0; slot != SignalIDCacheCapacity; slot++) {
    PTSanID id = Returns.IDs[slot];
    if (id) {
      IDs[Count++] = id;
      Returns.IDs[slot] = 0;
    }
  }
  Returns.Next = 0;
  Pool.returnIDs(IDs, Count);
}

void SignalSafeIDCache::release() {
  releaseUnusedIDs();
  drainReturns();
}

} // namespace __ptsan
