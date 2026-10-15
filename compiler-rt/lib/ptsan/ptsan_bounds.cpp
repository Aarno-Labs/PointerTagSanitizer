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
#include "sanitizer_common/sanitizer_allocator_internal.h"
#include "sanitizer_common/sanitizer_posix.h"
#include "sanitizer_common/sanitizer_stacktrace.h"

#include "interception/interception.h"

#include <assert.h>
#include <link.h>
#include <stdlib.h>
#include <sys/mman.h>

using namespace __ptsan;
using __sanitizer::StaticSpinMutex;

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_protect_global_initializers(void *start, void *end) {
  int result = __sanitizer::internal_mprotect(start, (uptr)end - (uptr)start,
                                             PROT_READ);
  CHECK_EQ(result, 0);
}

// Appended by value to instrumented variadic calls. An earlier
// collision deliberately gives smaller bounds, never access past this marker.
extern "C" SANITIZER_INTERFACE_ATTRIBUTE const u64 __ptsan_va_sentinel[2] = {
    0x8e7681bc94a7a1d3ULL, 0xb3e451a062dc7f19ULL};

static uptr vaStackSize(void *stack) {
  uptr start = get_addr(stack);
  uptr last = get_addr(GetCurrentThread()->stack_top()) -
              sizeof(__ptsan_va_sentinel);
  // Both the ABI stack slots and our byval marker are eight-byte aligned.
  // Stop at the last position where the complete marker fits on the stack.
  uptr cursor = start;
  for (; cursor <= last; cursor += sizeof(u64)) {
    const auto *words = (const u64 *)cursor;
    if (words[0] == __ptsan_va_sentinel[0] &&
        words[1] == __ptsan_va_sentinel[1]) {
      break;
    }
  }
  CHECK_LE(cursor, last);
  return cursor - start;
}

static void *tagVAStorage(void *base, uptr size, u16 id) {
  void *tagged = set_id(base, id);
  if (id != DefaultIDNo) {
    ptsan_bounds[id] = {(uptr)tagged, size};
  }
  return tagged;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_init_va_list_x86_64(void *storage, u64 register_id, u64 overflow_id,
                              uptr register_size) {
  struct VAList {
    unsigned gp_offset;
    unsigned fp_offset;
    void *overflow_arg_area;
    void *reg_save_area;
  };
  VAList *list = (VAList *)clear_id(storage);
  list->reg_save_area =
      tagVAStorage(list->reg_save_area, register_size, register_id);
  list->overflow_arg_area =
      tagVAStorage(list->overflow_arg_area, vaStackSize(list->overflow_arg_area),
                   overflow_id);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_init_va_list_aarch64(void *storage, u64 gp_id, u64 fp_id,
                               u64 stack_id) {
  struct VAList {
    void *stack;
    void *gr_top;
    void *vr_top;
    int gr_offs;
    int vr_offs;
  };
  VAList *list = (VAList *)clear_id(storage);
  // The top pointers are one-past the saved registers. Bounds describe the
  // region below each top, while va_arg adds a negative offset to the tagged
  // top. When a region is empty, the backend may leave its top uninitialized.
  if (list->gr_offs < 0) {
    void *base = (char *)list->gr_top + list->gr_offs;
    tagVAStorage(base, -(sptr)list->gr_offs, gp_id);
    list->gr_top = set_id(list->gr_top, gp_id);
  }
  if (list->vr_offs < 0) {
    void *base = (char *)list->vr_top + list->vr_offs;
    tagVAStorage(base, -(sptr)list->vr_offs, fp_id);
    list->vr_top = set_id(list->vr_top, fp_id);
  }
  list->stack = tagVAStorage(list->stack, vaStackSize(list->stack), stack_id);
}

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

struct SharedConstantRange {
  uptr begin;
  uptr end;
  u16 id;
};

// Ordinary ELF executables have only a handful of load segments. Keep this
// bootstrap state allocation-free and deliberately bounded: constants in any
// excess non-writable segments simply take the exact per-global ID path.
static constexpr uptr MaxSharedConstantRanges = 16;
static SharedConstantRange SharedConstantRanges[MaxSharedConstantRanges];
static uptr SharedConstantRangeCount;

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
  internal__exit(1);
}

// Callback to dl_iterate_phdr
static int record_shared_constant_ranges(struct dl_phdr_info *info, size_t,
                                         void *) {
  // Instrumented musl passes a tagged stack object to this runtime callback.
  // Its program-header pointer is a separate pointer and must also be raw.
  info = clear_id(info);
  const auto *phdrs = clear_id(info->dlpi_phdr);

  for (uptr index = 0; index < info->dlpi_phnum; ++index) {
    const ElfW(Phdr) &phdr = phdrs[index];
    if (phdr.p_type != PT_LOAD || (phdr.p_flags & PF_W) || !phdr.p_memsz) {
      continue;
    }
    const uptr begin = info->dlpi_addr + phdr.p_vaddr;
    const uptr end = begin + phdr.p_memsz;

    if (SharedConstantRangeCount == MaxSharedConstantRanges) {
      break;
    }

    SharedConstantRange &range =
        SharedConstantRanges[SharedConstantRangeCount++];
    range.begin = begin;
    range.end = end;
    range.id = ptsan_get_next_global_id((void *)begin, end - begin);
  }

  // Both supported libcs visit the main executable first. Stop here: sharing
  // for DSOs and their unload lifetime is not part of this implementation.
  return 1;
}

static void initialize_shared_constant_ranges() {
  CHECK(SharedConstantRangeCount == 0);

  // Called before global proxy constructors. Proxies initially contain raw
  // addresses, and strict bounds are deferred until initialization finishes.
  // Any future dl_iterate_phdr interceptor must preserve this bootstrap call.
  dl_iterate_phdr(record_shared_constant_ranges, nullptr);
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

  // Bootstrap uses ordinary bounds even in strict mode: instrumented libc
  // runs before its global proxies have IDs. ptsanInitFinish closes these
  // ranges after runtime setup and, for musl, the proxy constructors.
  ptsan_bounds[0].base = 0x10000;
  ptsan_bounds[0].span = 0x0000ffffffffffff;
  ptsan_bounds[DefaultIDNo].base = (u64)DefaultIDNo << 48;
  ptsan_bounds[DefaultIDNo].span = 0x0000ffffffffffff;

  // Discover shared constant ranges and allocate their permanent IDs before
  // any instrumented-libc global proxy constructors run. The complete table is
  // immutable after startup, so lookups require no synchronization.
  initialize_shared_constant_ranges();
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

extern "C" SANITIZER_INTERFACE_ATTRIBUTE u16
ptsan_get_constant_global_id(void *base, u64 sz) {
#ifdef PTSAN_NO_IDS
  return 0;
#else
  CHECK(base != nullptr);
  CHECK(sz != 0);

  for (uptr index = 0; index < SharedConstantRangeCount; ++index) {
    const SharedConstantRange &range = SharedConstantRanges[index];
    // Stored bounds include the ID, so test with this candidate range's ID.
    if (addr_in_range(set_id(base, range.id), range.id, sz)) {
      return range.id;
    }
  }

  return ptsan_get_next_global_id(base, sz);
#endif
}

// We should not be making non-writable-according-to-elf memory
// writable or else our global 'just overflow reads' scheme doesn't work
extern "C" SANITIZER_INTERFACE_ATTRIBUTE void
ptsan_check_mprotect(const void *address, u64 size, int protection) {
  if (!(protection & PROT_WRITE) || !size) {
    return;
  }

  const uptr begin = get_addr(address);
  const uptr end = begin + size;

  for (uptr index = 0; index < SharedConstantRangeCount; ++index) {
    const SharedConstantRange &range = SharedConstantRanges[index];
    if (begin >= range.end || range.begin >= end) {
      continue;
    }

    REPORT_ERROR("PTSan does not support making shared constant memory "
                 "writable: [0x%zx, 0x%zx) overlaps [0x%zx, 0x%zx)\n",
                 begin, end, range.begin, range.end);
    internal__exit(1);
  }
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
      internal__exit(1);
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

static u16 get_fallback_id() {
  if (ClStrictMode) {
    Printf("PTSan: ID space exhausted in strict mode\n");
    internal__exit(1);
  }
  return DefaultIDNo;
}

static void track_stack_ids(const u16 *ids, uptr n) {
  if (auto *thread = GetCurrentThread()) {
    for (uptr i = 0; i < n; ++i) {
      thread->slow_stack_ids.push_back(ids[i]);
    }
  }
}

static void track_stack_ids(u64 packed, unsigned n) {
  if (auto *thread = GetCurrentThread()) {
    for (unsigned i = 0; i < n; ++i) {
      thread->slow_stack_ids.push_back((u16)(packed >> (16 * i)));
    }
  }
}

static void untrack_stack_ids(uptr n) {
  if (auto *thread = GetCurrentThread()) {
    auto &ids = thread->slow_stack_ids;
    CHECK_LE(n, ids.size());
    // Remove freed IDs from the log; this keeps the allocated capacity.
    ids.resize(ids.size() - n);
  }
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
    return get_fallback_id();
  }

  u16 id = AllocationIDs[NextID];
  NextID = (NextID + 1) & IDsBufMask;

  #ifdef PTSAN_VALIDATE
  validate_stack_id_pre_write(&id, 1);
  #endif

  unlock_alloc_ids(locked);

  ptsan_bounds[id].base = tagged_base(base, id);
  ptsan_bounds[id].span = span;

  track_stack_ids(id, 1);
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

    u16 id = get_fallback_id();
    return pack2(id, id);
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

  u32 ids = pack2(id0, id1);
  track_stack_ids(ids, 2);
  return ids;
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
    u16 id = get_fallback_id();
    return pack3(id, id, id);
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

  u64 ids = pack3(id0, id1, id2);
  track_stack_ids(ids, 3);
  return ids;
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

    u16 id = get_fallback_id();
    return pack4(id, id, id, id);
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


  u64 ids = pack4(id0, id1, id2, id3);
  track_stack_ids(ids, 4);
  return ids;
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
    u16 id = get_fallback_id();
    for (u64 i = 0; i < n; i++) {
      ids[i] = id;
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
  track_stack_ids(ids, n);
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

// Recycling is shared with repeatable allocas and longjmp cleanup. Only the
// public fixed-stack helpers remove IDs from the fixed-stack tracking vector.
static void free_stack_id(u16 id) {
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
void ptsan_free_stack_ids_1(u16 id) {
#ifdef PTSAN_NO_IDS
  return;
#endif
  if (id != DefaultIDNo) {
    untrack_stack_ids(1);
  }
  free_stack_id(id);
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
  untrack_stack_ids(2);

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
  untrack_stack_ids(3);

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
  untrack_stack_ids(4);

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
static void free_stack_ids(u16 *ids, u64 n) {
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
void ptsan_free_stack_ids_n(u16 *ids, u64 n) {
#ifdef PTSAN_NO_IDS
  return;
#endif
  if (ids[0] != DefaultIDNo) {
    untrack_stack_ids(n);
  }
  free_stack_ids(ids, n);
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
    return get_fallback_id();
  }

  u16 id = AllocationIDs[NextID];
  NextID = (NextID + 1) & IDsBufMask;
  unlock_alloc_ids(locked);

  #ifdef PTSAN_VALIDATE
    CHECK(id > ptsan_stack_id_limit);
    if (id != 0 && id != DefaultIDNo) {
      if (ptsan_bounds[id].base != 0 || ptsan_bounds[id].span != 0) {
        Printf("About-to-malloc id is 0x%x. It should be clear\n", id);
        internal__exit(1);
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

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
RepeatableAllocaIDs *ptsan_get_repeatable_alloca_ids_container() {
  auto *thread = GetCurrentThread();
  CHECK(thread);
  auto *ids = (RepeatableAllocaIDs *)InternalAlloc(sizeof(RepeatableAllocaIDs));
  ids->Initialize(0);
  thread->repeatable_alloca_ids.push_back(ids);
  return ids;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
u16 ptsan_get_repeatable_alloca_id(RepeatableAllocaIDs *ids, void *base, u64 size) {
  u16 id = ptsan_get_next_nonstack_id(base, size, FREE_TY::NONE);
#ifdef PTSAN_VALIDATE
  // Keep default-ID requests too so statistics measure demand beyond capacity.
  if (id != 0) {
#else
  if (id != 0 && id != DefaultIDNo) {
#endif
    ids->push_back(id);
#ifdef PTSAN_VALIDATE
    // The general allocator accounts for total IDs; these are also stack objects.
    u64 used = __atomic_fetch_add(&__ptsan_used_stack_ids, 1, __ATOMIC_RELAXED) + 1;
    if (used > __ptsan_max_used_stack_ids) {
      __ptsan_max_used_stack_ids = used;
    }
#endif
  }
  return id;
}

static void free_repeatable_alloca_ids(RepeatableAllocaIDs *ids) {
#ifdef PTSAN_VALIDATE
  // The single-ID helper retires default requests without recycling their IDs.
  for (u16 id : *ids) {
    free_stack_id(id);
  }
#else
  if (!ids->empty()) {
    free_stack_ids(ids->data(), ids->size());
  }
#endif
  ids->Destroy();
  InternalFree(ids);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_free_repeatable_alloca_ids(RepeatableAllocaIDs *ids) {
  auto *thread = GetCurrentThread();
  CHECK(thread);
  CHECK_EQ(thread->repeatable_alloca_ids.back(), ids);
  thread->repeatable_alloca_ids.pop_back();
  free_repeatable_alloca_ids(ids);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_save_stack_id_checkpoint(StackIDCheckpoint *checkpoint) {
  auto *thread = GetCurrentThread();
  checkpoint->slow_ids = thread->slow_stack_ids.size();
  checkpoint->repeatable_allocas = thread->repeatable_alloca_ids.size();
}

void __ptsan::RestoreStackIDCheckpoint(PTSanThread &thread,
                                     const StackIDCheckpoint &checkpoint) {
  auto &containers = thread.repeatable_alloca_ids;
  while (containers.size() > checkpoint.repeatable_allocas) {
    auto *ids = containers.back();
    containers.pop_back();
    free_repeatable_alloca_ids(ids);
  }

  auto &ids = thread.slow_stack_ids;
  while (ids.size() > checkpoint.slow_ids) {
    free_stack_id(ids.back());
    ids.pop_back();
  }
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_restore_stack_id_checkpoint(const StackIDCheckpoint *checkpoint) {
  RestoreStackIDCheckpoint(*GetCurrentThread(), *checkpoint);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_validate_munmap(const void *address, u64 size) {
#ifdef PTSAN_NO_IDS
  // A no-ID runtime has no object metadata from which to validate the range.
  (void)address;
  (void)size;
  return;
#else
  // Let zero-length requests reach Linux so callers retain normal EINVAL
  // behavior without requiring a meaningful allocation pointer.
  if (size == 0) {
    return;
  }

  const u16 id = get_id(address);
  // DefaultIDNo is the deliberate degradation path after ID exhaustion. It
  // carries no exact object metadata, so preserve normal kernel behavior.
  if (id == DefaultIDNo) {
    return;
  }
  // munmap accepts the original byte count, but removes the rounded pages.
  if (id != 0 && ptsan_obj_types[id] == FREE_TY::MMAP &&
      ptsan_bounds[id].base == (u64)address &&
      ptsan_bounds[id].span == RoundUpTo(size, GetPageSizeCached())) {
    return;
  }

  REPORT_ERROR("PTSan rejected unsupported munmap before syscall: nonempty "
               "ranges must exactly match one tracked mmap allocation\n");
  internal__exit(1);
#endif
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_retire_mmap_if_exact(void *address, u64 size) {
#ifndef PTSAN_NO_IDS
  const u16 id = get_id(address);
  if (ptsan_obj_types[id] == FREE_TY::MMAP &&
      ptsan_bounds[id].base == (u64)address &&
      ptsan_bounds[id].span == RoundUpTo(size, GetPageSizeCached())) {
    ptsan_mark_ptr_freed(address, FREE_TY::MMAP);
  }
#else
  (void)address;
  (void)size;
#endif
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_validate_allocation(const void *ptr, FREE_TY expectedType) {
#ifndef PTSAN_NO_IDS
  const u16 id = get_id(ptr);
  if (id == 0 || id == DefaultIDNo) {
    return;
  }

  u64 current_base = ptsan_bounds[id].base;
  if (UNLIKELY(current_base != (u64)ptr)) {
    if (current_base == 0) {
      REPORT_ERROR("PTSan found double-free!\n");
      internal__exit(1);
    } else {
      REPORT_ERROR(
        "PTsan found invalid (maybe double, maybe something else?) free!. "
        "Base is 0x%llx, addr is 0x%llx, id is 0x%x",
        current_base, (u64)ptr, get_id(ptr)
      );
      internal__exit(1);
    }
  }

  if (UNLIKELY(ptsan_obj_types[id] != expectedType)) {
    REPORT_ERROR("PTSan found bad free of type %d on memory type %d\n", expectedType, ptsan_obj_types[id]);
    internal__exit(1);
  }
#endif
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
bool ptsan_mark_ptr_freed(void *ptr, FREE_TY freeType) {
  ptsan_validate_allocation(ptr, freeType);
#ifdef PTSAN_NO_IDS
  return true;
#else
  u16 id = get_id(ptr);
  if (id == 0) {
    return true;
  }
  if (id == DefaultIDNo) {
    #ifdef PTSAN_VALIDATE
      __atomic_fetch_sub(&__ptsan_used_ids, 1, __ATOMIC_RELAXED);
    #endif
    return true;
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
    internal__exit(1);
  }

  ptsan_handle_error(check_id, function_id, ptr_id, type);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
__attribute__((noinline))
void ptsan_complain_vector(u32 check_id, u64 function_id) {
  REPORT_ERROR("Found bad VECTOR access: #%d. Function ID: 0x%llx\n",
               check_id, function_id);

  if (ClAbortOnError) {
    internal__exit(1);
  }

  ptsan_handle_error(check_id, function_id, 0, "VECTOR");
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
