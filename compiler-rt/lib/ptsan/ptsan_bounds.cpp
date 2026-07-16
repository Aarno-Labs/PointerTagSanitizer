//===-- ptsan_bounds.cpp --------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of PointerTagSanitizer.
//
// PTSan object bounds tables and checks.
//
//===----------------------------------------------------------------------===//

#include "ptsan_bounds.h"
#include "ptsan_drift.h"
#include "ptsan_thread.h"
#include "ptsan_stats.h"
#include "ptsan_ptr_utils.h"

#include "sanitizer_common/sanitizer_mutex.h"
#include "sanitizer_common/sanitizer_stacktrace.h"

#include "interception/interception.h"

#include <assert.h>
#include <stdlib.h>

using namespace __ptsan;
using __sanitizer::StaticSpinMutex;

#ifdef PTSAN_VALIDATE
volatile u64 __ptsan_max_used_ids = 0;
volatile u64 __ptsan_used_ids = 0;
volatile u64 __ptsan_used_global_ids = 0;
volatile u64 __ptsan_used_stack_ids = 0;
volatile u64 __ptsan_max_used_stack_ids = 0;
#endif

// Ptr San Stuff

// Hold this when editing the allocation IDs structure,
// NextID, or LastID
// This is not perfect concurrency protection, but it should
// mostly work. There are two cases we need to deal with:
// - The actual allocation IDs structure gets
//   messed up
// - A give ObjectBounds is messed up
//
// The actual allocation IDs structure we care about,
// so we make sure to lock before messing with it.
//
// However, for individual IDs, I don't really think we do.
// If a given object is actually freed-and-accessed in two different
// threads in close-enough timing that we can run into an issue here,
// that is in fact UB, and giving a false positive (or negative) is
// not really a problem, since if the timing were very slightly
// different it would fall into the other category
//
// *theoretically*, if we run through an entire list of
// ids while allocating or deallocating an ID, we could
// hit a Bad Case, but I don't think that's really possible,
// and locking each ID would be expensive, so I'm leaving
// that alone for now.
static StaticSpinMutex AllocationIDsLock;

static u16 AllocationIDs[IDsBufSz];

// This is the HEAD of the circular buffer
static u32 NextID = 0;

// This is the TAIL of the circular buffer
static u32 IDTail =  NumValidIDs + 1;

ObjectBounds ptsan_bounds[MetadataNumIDs];
FREE_TY ptsan_obj_types[MetadataNumIDs];

ObjectBounds ptsan_default_id_bounds;
SANITIZER_INTERFACE_ATTRIBUTE THREADLOCAL u16 ptsan_next_stack_id;
SANITIZER_INTERFACE_ATTRIBUTE THREADLOCAL u16 ptsan_pending_stack_ids;
SANITIZER_INTERFACE_ATTRIBUTE THREADLOCAL u16 ptsan_stack_id_limit;
SANITIZER_INTERFACE_ATTRIBUTE u32 ptsan_active_stack_id_buckets;
StackIDBucket StackIDBuckets[MAX_STACK_ID_BUCKETS];


static bool lock_alloc_ids() {
  if (multithreaded) {
    AllocationIDsLock.Lock();
    return true;
  }

  return false;
}

static void unlock_alloc_ids(bool didLock) {
  if (didLock) {
    AllocationIDsLock.Unlock();
  }
}

static void report_invalid_stack_id_config(const char *Reason) {
  Printf("PTSan stack ID configuration is invalid: %s\n", Reason);
  Printf("  -ptsan-stack-ids-per-bucket=%u\n", ClStackIDsPerBucket);
  Printf("  -ptsan-stack-id-thread-buckets=%u\n", ClStackIDThreadBuckets);
  Printf("  max thread buckets: %u\n", MAX_STACK_ID_BUCKETS);
  Printf("  max fast-path stack ID limit: 0x%x\n", MAX_FAST_STACK_ID_LIMIT);
  Printf("  last valid runtime ID: 0x%x\n", LastIDNo);
  Abort();
}

void InitializeIDs() {
  u64 ActiveBuckets = ClStackIDThreadBuckets;
  u64 IDsPerBucket = ClStackIDsPerBucket;

  if (ActiveBuckets > MAX_STACK_ID_BUCKETS) {
    report_invalid_stack_id_config("requested too many stack ID buckets");
  }
  if (ActiveBuckets > 0 && IDsPerBucket == 0) {
    report_invalid_stack_id_config("nonzero stack ID buckets need IDs");
  }

  u64 ReservedStackIDs = ActiveBuckets * IDsPerBucket;
  if (ActiveBuckets != 0 && ReservedStackIDs / ActiveBuckets != IDsPerBucket) {
    report_invalid_stack_id_config("stack ID reservation overflows");
  }
  if (ReservedStackIDs > LastIDNo) {
    report_invalid_stack_id_config("reserved stack IDs exceed runtime ID space");
  }
  if (ReservedStackIDs + 1 > MAX_FAST_STACK_ID_LIMIT) {
    report_invalid_stack_id_config(
        "reserved stack IDs exceed fast-path-safe u16 range");
  }

  bool locked = lock_alloc_ids();
  ptsan_default_id_bounds.base = 0x10000; // filter out Obviously Bad addresses
  ptsan_default_id_bounds.span = 0x0000ffffffffffff;

  ptsan_active_stack_id_buckets = (u32)ActiveBuckets;
  NextID = (u32)ReservedStackIDs + 1;

  for (u32 i = 0; i < MAX_STACK_ID_BUCKETS; i++) {
    StackIDBucket &B = StackIDBuckets[i];
    B.avaliable = i < ActiveBuckets;
    if (i < ActiveBuckets) {
      // 1 - 0x201, 0x201-0x401, etc...
      // end is not included
      B.start = (u16)(i * IDsPerBucket + 1);
      B.end = (u16)(B.start + IDsPerBucket);
    } else {
      B.start = 0;
      B.end = 0;
    }
  }

  if (ActiveBuckets > 0) {
    CHECK(StackIDBuckets[ActiveBuckets - 1].end == NextID);
  } else {
    CHECK(NextID == 1);
  }

  // Don't initialize anything >= LastID or < NextID
  for (u32 i = NextID; i < IDTail; i++) {
    AllocationIDs[i] = i;
  }

  unlock_alloc_ids(locked);

  if (ClStrictMode) {
    ptsan_bounds[0].base = 0;
    ptsan_bounds[0].span = 0;
    ptsan_bounds[DefaultIDNo].base = 0;
    ptsan_bounds[DefaultIDNo].span = 0;
  } else {
    // ID 0 and DefaultIDNo addresses are always valid, so
    // set accordingly. However, we want to capture
    // *obviously* garbage pointers, so we do have a
    // non-zero bound
    ptsan_bounds[0].base = 0x10000;
    ptsan_bounds[0].span = 0x0000ffffffffffff;
    ptsan_bounds[DefaultIDNo].base = (u64)DefaultIDNo << 48;
    ptsan_bounds[DefaultIDNo].span = 0x0000ffffffffffff;
  }
}

DECLARE_REAL(void *, malloc, SIZE_T size)
DECLARE_REAL(void, free, void *ptr)

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
u16 ptsan_get_next_global_id(void *base,  u64 sz) {
#ifdef PTSAN_NO_IDS
  return 0;
#else
#ifdef PTSAN_VALIDATE
  __atomic_fetch_add(&__ptsan_used_global_ids, 1, __ATOMIC_RELAXED);
#endif
  u16 id = ptsan_get_next_nonstack_id(base, sz, FREE_TY::NONE);
  assert(id != DefaultIDNo);
  return id;
#endif
}

#ifdef PTSAN_VALIDATE
static inline void validate_stack_id_start(u64 n) {

  u64 old_used_ids = __atomic_fetch_add(&__ptsan_used_ids, n, __ATOMIC_RELAXED);
  u64 old_used_stack_ids = __atomic_fetch_add(&__ptsan_used_stack_ids, n, __ATOMIC_RELAXED);

  CHECK(n > 0);
  // so 1 is 0, 2 is 1, 3 is 2, 4 is 3, and >4 is 4
  u32 fn_call_bucket = n > 4 ? 4 : n - 1;
  __atomic_fetch_add(ptsan_n_alloca_fn_calls + fn_call_bucket , 1, __ATOMIC_RELAXED);

  // This is a race condition, and we don't care
  if (old_used_ids + n > __ptsan_max_used_ids) {
    __ptsan_max_used_ids = old_used_ids + n;
  }

  if (old_used_stack_ids + n > __ptsan_max_used_stack_ids) {
    __ptsan_max_used_stack_ids = old_used_stack_ids + n;
  }

  // Runtime stack ID helpers are used both when the inline stack fast path
  // overflows and when instrumentation deliberately disables that fast path.
}
#endif

#ifdef PTSAN_VALIDATE
static inline void validate_stack_id_pre_write(u16 *ids, u64 n) {
  u64 pid = internal_getpid();
  //Printf("%llu: Function needs %llu stack ids:\n", pid, n);
  for (u64 i = 0; i < n; i++) {
    u16 id = ids[i];
    CHECK(id != 0 && id != DefaultIDNo && id > ptsan_stack_id_limit);

    //Printf("\t%llu: Checking id 0x%x\n", pid, id);

    if (ptsan_bounds[id].base != 0 || ptsan_bounds[id].span != 0) {
      Printf("%llu: Stack-allocated id is 0x%x. It should be clear\n", pid, id);
      Abort();
    }
  }
}

static inline void validate_stack_id_post_write(u16 *ids, u64 n) {
  u64 pid = internal_getpid();
  for (u64 i = 0; i < n; i++) {
    u16 id = ids[i];

    if (ptsan_bounds[id].base == 0 || ptsan_bounds[id].span == 0) {
      Printf("%llu: Stack-allocated id is 0x%x. It should now be dirty\n", pid, id);
      Abort();
    }
  }
}
#endif

static inline u32 get_avail_ids() {
  u32 head = NextID;
  u32 tail = IDTail;

  // Number of valid IDs in the ring. This is a bit confusing, but:
  // - If tail == head this is 0
  // - If tail > head, this is tail - head, which is correct
  // - If tail < head, this will *wrap*. Since these are all u32s, we'll get
  //   2^32 - (head - tail). The `& mask` is the same as `% IDsBufSz`, so we end
  //   up with IDsBufSz - head + tail, which is the correct value!
  return (tail - head) & IDsBufMask;
}

static inline u32 pack2(u16 a, u16 b) {
  return (u32)a | ((u32)b << 16);
}

static inline u64 pack3(u16 a, u16 b, u16 c) {
  return (u64)a | ((u64)b << 16) | ((u64)c << 32);
}

static inline u64 pack4(u16 a, u16 b, u16 c, u16 d) {
  return (u64)a | ((u64)b << 16) | ((u64)c << 32) | ((u64)d << 48);
}

static inline uptr tagged_base(void *base, u16 id) {
  return ((uptr)id << 48) | (uptr)base;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
u16 ptsan_get_stack_ids_1(void * base, u64 span) {
#ifdef PTSAN_NO_IDS
  return 0;
#endif

#ifdef PTSAN_VALIDATE
  validate_stack_id_start(1);
#endif

  bool locked = lock_alloc_ids();
  u32 avail = get_avail_ids();

  if (UNLIKELY(1 > avail)) {
    unlock_alloc_ids(locked);
    return DefaultIDNo;
  }

  u16 id = AllocationIDs[NextID];
  NextID = (NextID + 1) & IDsBufMask;

  #ifdef PTSAN_VALIDATE
  validate_stack_id_pre_write(&id, 1);
  #endif

  unlock_alloc_ids(locked);

  ptsan_bounds[id].base = tagged_base(base, id);
  ptsan_bounds[id].span = span;

  #ifdef PTSAN_VALIDATE
  validate_stack_id_post_write(&id, 1);
  #endif

  return id;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
u32 ptsan_get_stack_ids_2(void *base0, void *base1, u64 spans01) {
#ifdef PTSAN_NO_IDS
  return 0;
#endif

#ifdef PTSAN_VALIDATE
  validate_stack_id_start(2);
#endif

  u32 span0 = (u32)spans01;
  u32 span1 = (u32)(spans01 >> 32);

  bool locked = lock_alloc_ids();
  u32 avail = get_avail_ids();

  if (UNLIKELY(2 > avail)) {
    unlock_alloc_ids(locked);

    return pack2(DefaultIDNo, DefaultIDNo);
  }

  u16 id0 = AllocationIDs[NextID];
  NextID = (NextID + 1) & IDsBufMask;
  u16 id1 = AllocationIDs[NextID];
  NextID = (NextID + 1) & IDsBufMask;

  #ifdef PTSAN_VALIDATE
  u16 tmp[2] = {id0, id1};
  validate_stack_id_pre_write(tmp, 2);
  #endif

  unlock_alloc_ids(locked);

  ptsan_bounds[id0].base = tagged_base(base0, id0);
  ptsan_bounds[id0].span = span0;
  ptsan_bounds[id1].base = tagged_base(base1, id1);
  ptsan_bounds[id1].span = span1;

  #ifdef PTSAN_VALIDATE
  validate_stack_id_post_write(tmp, 2);
  #endif

  return pack2(id0, id1);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
u64 ptsan_get_stack_ids_3(void *base0, void *base1, void *base2, u64 spans01, u32 span2) {

#ifdef PTSAN_NO_IDS
  return 0;
#endif

#ifdef PTSAN_VALIDATE
  validate_stack_id_start(3);
#endif

  u32 span0 = (u32)spans01;
  u32 span1 = (u32)(spans01 >> 32);

  bool locked = lock_alloc_ids();
  u32 avail = get_avail_ids();

  if (UNLIKELY(3 > avail)) {
    unlock_alloc_ids(locked);
    return pack3(DefaultIDNo, DefaultIDNo, DefaultIDNo);
  }

  u16 id0= AllocationIDs[NextID];
  NextID = (NextID + 1) & IDsBufMask;
  u16 id1 = AllocationIDs[NextID];
  NextID = (NextID + 1) & IDsBufMask;
  u16 id2 = AllocationIDs[NextID];
  NextID = (NextID + 1) & IDsBufMask;

  #ifdef PTSAN_VALIDATE
  u16 tmp[3] = {id0, id1, id2};
  validate_stack_id_pre_write(tmp, 3);
  #endif

  unlock_alloc_ids(locked);

  ptsan_bounds[id0].base = tagged_base(base0, id0);
  ptsan_bounds[id0].span = span0;
  ptsan_bounds[id1].base = tagged_base(base1, id1);
  ptsan_bounds[id1].span = span1;
  ptsan_bounds[id2].base = tagged_base(base2, id2);
  ptsan_bounds[id2].span = span2;

  #ifdef PTSAN_VALIDATE
  validate_stack_id_post_write(tmp, 3);
  #endif

  return pack3(id0, id1, id2);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
u64 ptsan_get_stack_ids_4(void *base0, void *base1, void *base2, void *base3,
                          u64 spans01, u64 spans23) {
#ifdef PTSAN_NO_IDS
  return 0;
#endif

#ifdef PTSAN_VALIDATE
  validate_stack_id_start(4);
#endif
  u32 span0 = (u32)spans01;
  u32 span1 = (u32)(spans01 >> 32);
  u32 span2 = (u32)spans23;
  u32 span3 = (u32)(spans23 >> 32);

  bool locked = lock_alloc_ids();
  u32 avail = get_avail_ids();

  if (UNLIKELY(4 > avail)) {
    unlock_alloc_ids(locked);

    return pack4(DefaultIDNo, DefaultIDNo, DefaultIDNo, DefaultIDNo);
  }

  u16 id0= AllocationIDs[NextID];
  NextID = (NextID + 1) & IDsBufMask;
  u16 id1 = AllocationIDs[NextID];
  NextID = (NextID + 1) & IDsBufMask;
  u16 id2 = AllocationIDs[NextID];
  NextID = (NextID + 1) & IDsBufMask;
  u16 id3 = AllocationIDs[NextID];
  NextID = (NextID + 1) & IDsBufMask;

  #ifdef PTSAN_VALIDATE
  u16 tmp[4] = {id0, id1, id2, id3};
  validate_stack_id_pre_write(tmp, 4);
  #endif

  unlock_alloc_ids(locked);

  ptsan_bounds[id0].base = tagged_base(base0, id0);
  ptsan_bounds[id0].span = span0;
  ptsan_bounds[id1].base = tagged_base(base1, id1);
  ptsan_bounds[id1].span = span1;
  ptsan_bounds[id2].base = tagged_base(base2, id2);
  ptsan_bounds[id2].span = span2;
  ptsan_bounds[id3].base = tagged_base(base3, id3);
  ptsan_bounds[id3].span = span3;


  #ifdef PTSAN_VALIDATE
  validate_stack_id_post_write(tmp, 4);
  #endif

  return pack4(id0, id1, id2, id3);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_get_stack_ids_n(u16 *ids, u64 n) {
#ifdef PTSAN_NO_IDS
  internal_memset(ids, 0, n * sizeof(u16));
  return;
#endif

#ifdef PTSAN_VALIDATE
  validate_stack_id_start(n);
#endif

  bool locked = lock_alloc_ids();

  u32 avail = get_avail_ids();

  if (UNLIKELY(n > avail)) {
    unlock_alloc_ids(locked);
    for (u64 i = 0; i < n; i++) {
      ids[i] = DefaultIDNo;
    }
    return;
  }

  // First contiguous run from head to end of buffer
  u32 first = IDsBufSz - NextID;
  if (first > n) {
    first = n;
  }

  // Copy first chunk
  internal_memcpy(ids,
                  AllocationIDs + NextID,
                  (size_t)first * sizeof(u16));

  // If needed, wrap and copy the remainder
  u32 rem = n - first;
  if (rem) {
    internal_memcpy(ids + first,
                    AllocationIDs,
                    (size_t)rem * sizeof(u16));
  }

  // Advance head
  NextID = (NextID + n) & IDsBufMask;

  #ifdef PTSAN_VALIDATE
  validate_stack_id_pre_write(ids, n);
  #endif

  unlock_alloc_ids(locked);
}

#ifdef PTSAN_VALIDATE
static void validate_stack_id_free(u16 id0, u64 n) {
  __atomic_fetch_sub(&__ptsan_used_ids, n, __ATOMIC_RELAXED);
  __atomic_fetch_sub(&__ptsan_used_stack_ids, n, __ATOMIC_RELAXED);

  // Stack ids that make it here should always be 'real'
  if(id0 <= ptsan_stack_id_limit) {
    Printf("Ids[0] = %d\n", id0);
  }
  CHECK(id0 > ptsan_stack_id_limit);

  // We should always have enough free capacity
  u32 free_cap = (NextID - IDTail - 1) & IDsBufMask;
  CHECK(free_cap >= n);
}
#endif

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_free_stack_ids_1(u16 id) {
#ifdef PTSAN_NO_IDS
  return;
#endif
#ifdef PTSAN_VALIDATE
  validate_stack_id_free(id, 1);
#endif
  if (id == DefaultIDNo) {
    return;
  }

  ptsan_bounds[id].base = 0;
  ptsan_bounds[id].span = 0;

  bool locked = lock_alloc_ids();
  AllocationIDs[IDTail] = id;
  IDTail = (IDTail + 1) & IDsBufMask;
  unlock_alloc_ids(locked);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_free_stack_ids_2(u32 packed) {
#ifdef PTSAN_NO_IDS
  return;
#endif

  u16 id0 = (u16)(packed);
  u16 id1 = (u16)(packed >> 16);

#ifdef PTSAN_VALIDATE
  validate_stack_id_free(id0, 2);
#endif

  if (id0 == DefaultIDNo) {
    return;
  }

  ptsan_bounds[id0].base = 0;
  ptsan_bounds[id0].span = 0;
  ptsan_bounds[id1].base = 0;
  ptsan_bounds[id1].span = 0;

  bool locked = lock_alloc_ids();
  AllocationIDs[IDTail] = id0;
  IDTail = (IDTail + 1) & IDsBufMask;
  AllocationIDs[IDTail] = id1;
  IDTail = (IDTail + 1) & IDsBufMask;
  unlock_alloc_ids(locked);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_free_stack_ids_3(u64 packed) {
#ifdef PTSAN_NO_IDS
  return;
#endif

  u16 id0 = (u16)(packed);
  u16 id1 = (u16)(packed >> 16);
  u16 id2 = (u16)(packed >> 32);

#ifdef PTSAN_VALIDATE
  validate_stack_id_free(id0, 3);
#endif

  if (id0 == DefaultIDNo) {
    return;
  }

  ptsan_bounds[id0].base = 0;
  ptsan_bounds[id0].span = 0;
  ptsan_bounds[id1].base = 0;
  ptsan_bounds[id1].span = 0;
  ptsan_bounds[id2].base = 0;
  ptsan_bounds[id2].span = 0;

  bool locked = lock_alloc_ids();
  AllocationIDs[IDTail] = id0;
  IDTail = (IDTail + 1) & IDsBufMask;
  AllocationIDs[IDTail] = id1;
  IDTail = (IDTail + 1) & IDsBufMask;
  AllocationIDs[IDTail] = id2;
  IDTail = (IDTail + 1) & IDsBufMask;
  unlock_alloc_ids(locked);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_free_stack_ids_4(u64 packed) {
#ifdef PTSAN_NO_IDS
  return;
#endif

  u16 id0 = (u16)(packed);
  u16 id1 = (u16)(packed >> 16);
  u16 id2 = (u16)(packed >> 32);
  u16 id3 = (u16)(packed >> 48);

#ifdef PTSAN_VALIDATE
  validate_stack_id_free(id0, 4);
#endif

  if (id0 == DefaultIDNo) {
    return;
  }

  ptsan_bounds[id0].base = 0;
  ptsan_bounds[id0].span = 0;
  ptsan_bounds[id1].base = 0;
  ptsan_bounds[id1].span = 0;
  ptsan_bounds[id2].base = 0;
  ptsan_bounds[id2].span = 0;
  ptsan_bounds[id3].base = 0;
  ptsan_bounds[id3].span = 0;

  bool locked = lock_alloc_ids();
  AllocationIDs[IDTail] = id0;
  IDTail = (IDTail + 1) & IDsBufMask;
  AllocationIDs[IDTail] = id1;
  IDTail = (IDTail + 1) & IDsBufMask;
  AllocationIDs[IDTail] = id2;
  IDTail = (IDTail + 1) & IDsBufMask;
  AllocationIDs[IDTail] = id3;
  IDTail = (IDTail + 1) & IDsBufMask;
  unlock_alloc_ids(locked);
}

// Given [12, 14, 15, 66], LastID_old = 12, each of the internals
// to the next, and LastID_new to 66. NextID = 12 if it was null
extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_free_stack_ids_n(u16 *ids, u64 n) {
#ifdef PTSAN_NO_IDS
  return;
#endif
#ifdef PTSAN_VALIDATE
  validate_stack_id_free(ids[0], n);
#endif

  u16 id = ids[0];

  // Ids only come in all-valid or no-valid, so we can just check this once
  // here. If it's default, we're done. If not, none of them are default.
  if (id == DefaultIDNo) {
    return;
  }

  // Clear IDs
  // This doesn't need to be locked because if another
  // thread has a UAF with this thread, it's weird nondeterminism
  // anyway
  for (SIZE_T i = 0; i < n; i++) {
    id = ids[i];
    ptsan_bounds[id].base = 0;
    ptsan_bounds[id].span = 0;
  }

  bool locked = lock_alloc_ids();

  u32 head = NextID;
  u32 tail = IDTail;

  // first contiguous chunk: from tail to end of buffer
  u32 first = IDsBufSz - tail;
  if (first > n) first = n;

  // copy first chunk (bytes!)
  internal_memcpy(AllocationIDs + tail, ids, (size_t)first * sizeof(u16));

  // wrap remainder if needed
  u32 rem = n - first;
  if (rem) {
    internal_memcpy(AllocationIDs, ids + first, (size_t)rem * sizeof(u16));
  }

  // advance tail
  IDTail = (tail + n) & IDsBufMask;

  unlock_alloc_ids(locked);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
u16 ptsan_get_next_nonstack_id(void *base, u64 sz, FREE_TY type) {
#ifdef PTSAN_NO_IDS
  return 0;
#else
#ifdef PTSAN_VALIDATE
  static bool announcedOverflow = false;
  u64 old_used_ids = __atomic_fetch_add(&__ptsan_used_ids, 1, __ATOMIC_RELAXED);
  if (old_used_ids + 1 > __ptsan_max_used_ids) {
    __ptsan_max_used_ids = old_used_ids + 1;
  }
#endif

  bool locked = lock_alloc_ids();
  // Out of IDs
  if (NextID == IDTail) {
    #ifdef PTSAN_VALIDATE
    if (!announcedOverflow) {
      //Printf("Ran out of IDs for a malloc variable!\n");
      announcedOverflow = true;
    }
    #endif
    unlock_alloc_ids(locked);
    return DefaultIDNo;
  }

  u16 id = AllocationIDs[NextID];
  NextID = (NextID + 1) & IDsBufMask;
  unlock_alloc_ids(locked);

  #ifdef PTSAN_VALIDATE
    CHECK(id > ptsan_stack_id_limit);
    if (id != 0 && id != DefaultIDNo) {
      if (ptsan_bounds[id].base != 0 || ptsan_bounds[id].span != 0) {
        Printf("About-to-malloc id is 0x%x. It should be clear\n", id);
        Abort();
      }
    }
  #endif

  // I think *theoretically* an entire span
  // of ids could be cycled through in a different
  // thread before we get here, but... I don't think
  // that can actually happen?
  ptsan_bounds[id].base = ((u64)id << 48) | (u64)base;
  ptsan_bounds[id].span = sz;
  ptsan_obj_types[id] = type;
  //Printf("Allocating ptsan id 0x%x with size 0x%llx\n", out->id, sz);

  return id;
#endif
}

// False if invalid
extern "C" SANITIZER_INTERFACE_ATTRIBUTE
bool ptsan_mark_ptr_freed(void *ptr, FREE_TY freeType) {
#ifdef PTSAN_NO_IDS
  return true;
#else
  u16 id = get_id(ptr);

  if (id == 0) {
    return true;
  }

  //Printf("Freeing malloc ptsan id 0x%x for ptr %p\n", id, clear_id(ptr));

  if (id == DefaultIDNo) {
    #ifdef PTSAN_VALIDATE
      __atomic_fetch_sub(&__ptsan_used_ids, 1, __ATOMIC_RELAXED);
    #endif
    return true;
  }

  u64 current_base = ptsan_bounds[id].base;
  if (UNLIKELY(current_base != (u64)ptr)) {
    if (current_base == 0) {
      REPORT_ERROR("PTSan found double-free!\n");
      Abort();
    } else {
      REPORT_ERROR(
        "PTsan found invalid (maybe double, maybe something else?) free!. "
        "Base is 0x%llx, addr is 0x%llx, id is 0x%x",
        current_base, (u64)ptr, get_id(ptr)
      );
      Abort();
    }
  }

  if (UNLIKELY(ptsan_obj_types[id] != freeType)) {
    REPORT_ERROR("PTSan found bad free of type %d on memory type %d\n", freeType, ptsan_obj_types[id]);
    Abort();
  }

  ptsan_bounds[id].base = 0;
  ptsan_bounds[id].span = 0;
  ptsan_obj_types[id] = FREE_TY::NONE;

  bool locked = lock_alloc_ids();
  AllocationIDs[IDTail] = id;
  IDTail = (IDTail + 1) & IDsBufMask;
  unlock_alloc_ids(locked);

  #ifdef PTSAN_VALIDATE
  __atomic_fetch_sub(&__ptsan_used_ids, 1, __ATOMIC_RELAXED);
  #endif
  return true;
#endif
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
bool ptsan_check(void *ptr, u64 sz) {
  u16 id = get_id(ptr);
  return addr_in_range(ptr, id, sz);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
__attribute__((noinline))
void ptsan_complain(u32 check_id, u64 function_id, u64 ptr_id,
                    const char * type, u64 min, u64 size) {
  // size-0 illegal accesses are fine, actually
  if(size == 0) {
    return;
  }

  u64 base = ptsan_bounds[ptr_id].base;
  u64 span = ptsan_bounds[ptr_id].span;

  REPORT_ERROR("Found bad %s access: #%d. Function ID: 0x%llx, Ptr ID: 0x%llx\n\t\t"
         "Base: 0x%llx, Bound: 0x%llx (Alloc size  0x%llx)\n\t\t"
         "Min:  0x%llx, Max:   0x%llx (Access size 0x%llx),\n\t\t"
         "Min-Base = %lld, Span-Size = %lld\n",
         type, check_id, function_id, ptr_id, base, base + span,
         span, min, min + size, size, min - base, span - size);

  if (ClAbortOnError) {
    Abort();
  }

  ptsan_handle_error(check_id, function_id, ptr_id, type);
}

// XXX: For a production/'real security' release, we
// probably want to compile this out since it leaks
// information that one could use to dodge our security
//
// At least it's set up so a caller can't edit values
#ifndef PTSAN_PRODUCTION_MODE
extern "C" SANITIZER_INTERFACE_ATTRIBUTE
u64 ptsan_get_bound(u16 id) {
  return ptsan_bounds[id].base + ptsan_bounds[id].span;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
u64 ptsan_get_base(u16 id) {
  return ptsan_bounds[id].base;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
u64 ptsan_get_size(u16 id) {
  return ptsan_bounds[id].span;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
u64 ptsan_id_bits() {
  return PTSAN_ID_BITS;
}
#endif
