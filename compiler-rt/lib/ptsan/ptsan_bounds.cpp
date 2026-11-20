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
#include "ptsan_origins.h"
#include "ptsan_id_cache.h"
#include "ptsan_signal.h"
#include "ptsan_drift.h"
#include "ptsan_thread.h"
#include "ptsan_stats.h"
#include "ptsan_ptr_utils.h"
#include "ptsan_random.h"

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

#ifdef PTSAN_32_BIT_IDS
static void write_bounds(PTSanID id, void *base, u64 size, u32 payload_start) {
  if (UNLIKELY(size >= (1ULL << 31))) {
    Printf("PTSan ID32 cannot represent objects of 2 GiB or larger\n");
    internal__exit(1);
  }
  ptsan_origins[id] = payload_start;
  ptsan_bounds[id] = {(u64)base - payload_start, size};
}
#else
static void write_bounds(PTSanID id, void *base, u64 size) {
  if (isDefaultID(id)) {
    return;
  }
  ptsan_bounds[id] = {((u64)id << 48) | (u64)base, size};
}
#endif

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
  uptr start = (uptr)stack;
  // Both the ABI stack slots and our byval marker are eight-byte aligned.
  // Instrumented callers append the sentinel after their stack arguments.
  for (uptr cursor = start;; cursor += sizeof(u64)) {
    const auto *words = (const u64 *)cursor;
    if (words[0] == __ptsan_va_sentinel[0] &&
        words[1] == __ptsan_va_sentinel[1]) {
      return cursor - start;
    }
  }
}

// Each va_start initializes a list, but all lists in one invocation share
// the backing-storage IDs.
#ifdef PTSAN_32_BIT_IDS
static void registerVAStorage(void *base, uptr size, PTSanID id) {
  // A matching size and zero offset mean this ID already describes this region.
  // Keep its origin: choosing a new one would invalidate pointers in other lists.
  if (ptsan_bounds[id].span == size &&
      get_object_offset(base, id, Polarity::Stripped) == 0) {
    return;
  }

  uint64_t origin = choose_id32_payload_start((u64)base, sizeof(u64));
  write_bounds(id, base, size, origin);
}
#else
static void registerVAStorage(void *base, uptr size, PTSanID id) {
  write_bounds(id, base, size);
}
#endif

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_init_va_list_x86_64(void *storage, u64 register_id, u64 overflow_id,
                              uptr register_size) {
  struct VAList {
    unsigned gp_offset;
    unsigned fp_offset;
    void *overflow_arg_area;
    void *reg_save_area;
  };
  // Lowering passes raw va_start storage containing backend-written addresses.
  VAList *list = (VAList *)storage;
  registerVAStorage(list->reg_save_area, register_size, register_id);
  registerVAStorage(list->overflow_arg_area, vaStackSize(list->overflow_arg_area),
                    overflow_id);
  list->reg_save_area = set_id(list->reg_save_area, register_id);
  list->overflow_arg_area = set_id(list->overflow_arg_area, overflow_id);
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
  // Lowering passes raw va_start storage containing backend-written addresses.
  VAList *list = (VAList *)storage;
  // The top pointers are one-past the saved registers. Bounds describe the
  // region below each top, while va_arg adds a negative offset to the tagged
  // top. When a region is empty, the backend may leave its top uninitialized.
  if (list->gr_offs < 0) {
    void *base = (char *)list->gr_top + list->gr_offs;
    registerVAStorage(base, -(sptr)list->gr_offs, gp_id);
    list->gr_top = set_id(list->gr_top, gp_id);
  }
  if (list->vr_offs < 0) {
    void *base = (char *)list->vr_top + list->vr_offs;
    registerVAStorage(base, -(sptr)list->vr_offs, fp_id);
    list->vr_top = set_id(list->vr_top, fp_id);
  }
  registerVAStorage(list->stack, vaStackSize(list->stack), stack_id);
  list->stack = set_id(list->stack, stack_id);
}

#ifdef PTSAN_VALIDATE
volatile u64 __ptsan_max_used_ids = 0;
volatile u64 __ptsan_used_ids = 0;
volatile u64 __ptsan_used_global_ids = 0;
volatile u64 __ptsan_used_stack_ids = 0;
volatile u64 __ptsan_max_used_stack_ids = 0;
#endif

struct SharedConstantRange {
  uptr begin;
  uptr end;
  PTSanID id;
};

// Ordinary ELF executables have only a handful of load segments. Keep this
// bootstrap state allocation-free and deliberately bounded: constants in any
// excess non-writable segments simply take the exact per-global ID path.
static constexpr uptr MaxSharedConstantRanges = 16;
static SharedConstantRange SharedConstantRanges[MaxSharedConstantRanges];
static uptr SharedConstantRangeCount;

#ifdef PTSAN_32_BIT_IDS
ObjectBounds *ptsan_bounds;
FREE_TY *ptsan_obj_types;
#else
ObjectBounds ptsan_bounds[MetadataNumIDs];
FREE_TY ptsan_obj_types[MetadataNumIDs];
#endif

ObjectBounds ptsan_default_id_bounds;
SANITIZER_INTERFACE_ATTRIBUTE THREADLOCAL u64 ptsan_next_stack_id;
SANITIZER_INTERFACE_ATTRIBUTE THREADLOCAL PTSanID ptsan_pending_stack_ids;
SANITIZER_INTERFACE_ATTRIBUTE THREADLOCAL u64 ptsan_stack_id_limit;
SANITIZER_INTERFACE_ATTRIBUTE THREADLOCAL u8 ptsan_in_signal_context;
StackIDBucket *StackIDBuckets;

#ifdef PTSAN_VALIDATE
static bool is_fast_stack_id(PTSanID ID) {
  // The shared claimed bitmap includes heap/global chunks too. Ownership is
  // only needed for these validation checks, so inspect the stack descriptors.
  for (u32 I = 0; I < ClStackIDThreadBuckets; ++I) {
    const auto &Bucket = StackIDBuckets[I];
    if (ID >= Bucket.start && ID < Bucket.end) {
      return true;
    }
  }
  return false;
}
#endif

static void initialize_stack_id_buckets() {
  if (!ClStackIDThreadBuckets) {
    return;
  }
  CHECK_LE((uptr)ClStackIDThreadBuckets, ~(uptr)0 / sizeof(*StackIDBuckets));
  StackIDBuckets = (StackIDBucket *)MmapNoReserveOrDie(
      (uptr)ClStackIDThreadBuckets * sizeof(*StackIDBuckets),
      "PTSan stack ID buckets");
  for (u32 I = 0; I < ClStackIDThreadBuckets; ++I) {
    IDBucket Bucket = claimIDBucket();
    StackIDBuckets[I] = {true, Bucket.start, Bucket.end};
  }
}

// Callback to dl_iterate_phdr
static int record_shared_constant_ranges(struct dl_phdr_info *info, size_t,
                                         void *) {
  // Both the host loader callback and musl's early ELF scan supply raw data.
  const auto *phdrs = info->dlpi_phdr;

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
    range.id = ptsan_get_next_global_id((void *)begin, end - begin,
                                       GetPageSizeCached());
  }

  // Both supported libcs visit the main executable first. Stop here: sharing
  // for DSOs and their unload lifetime is not part of this implementation.
  return 1;
}

#ifdef PTSAN_MUSL_INTERCEPTION
void InitializeSharedConstantRanges() {
  CHECK(SharedConstantRangeCount == 0);
  // Static musl bootstrap cannot call instrumented dl_iterate_phdr before TLS.
  // Derive the load bias from the segment containing the linker-supplied header.
  extern const ElfW(Ehdr) __ehdr_start
      __attribute__((visibility("hidden")));
  const auto *phdrs = (const ElfW(Phdr) *)(
      (uptr)&__ehdr_start + __ehdr_start.e_phoff);
  CHECK_EQ(__ehdr_start.e_phentsize, sizeof(*phdrs));
  dl_phdr_info info = {};
  info.dlpi_phdr = phdrs;
  info.dlpi_phnum = __ehdr_start.e_phnum;
  bool found = false;
  for (uptr i = 0; i < info.dlpi_phnum; ++i) {
    if (phdrs[i].p_type == PT_LOAD && phdrs[i].p_offset == 0) {
      info.dlpi_addr = (uptr)&__ehdr_start - phdrs[i].p_vaddr;
      found = true;
      break;
    }
  }
  CHECK(found);
  record_shared_constant_ranges(&info, sizeof(info), nullptr);
}
#else
void InitializeSharedConstantRanges() {
  CHECK(SharedConstantRangeCount == 0);
  // Called before global proxy constructors. Proxies initially contain raw
  // addresses, and strict bounds are deferred until initialization finishes.
  // Any future dl_iterate_phdr interceptor must preserve this bootstrap call.
  dl_iterate_phdr(record_shared_constant_ranges, nullptr);
}
#endif

void InitializeIDs() {
  ScopedSignalBlock block;
#ifdef PTSAN_32_BIT_IDS
  initializeOrigins();
  // Large BSS reservations can be rejected before program entry. Reserve
  // ID32 tables without that commit charge; pages are populated as used.
  ptsan_bounds = (ObjectBounds *)MmapNoReserveOrDie(
      MetadataNumIDs * sizeof(*ptsan_bounds), "PTSan bounds");
  ptsan_origins = (uint32_t *)MmapNoReserveOrDie(
      MetadataNumIDs * sizeof(*ptsan_origins), "PTSan origins");
  ptsan_obj_types = (FREE_TY *)MmapNoReserveOrDie(
      MetadataNumIDs * sizeof(*ptsan_obj_types), "PTSan object types");
#endif
  CHECK_LE(ClIDReuseDistance,
           (u64)LastIDNo + 1 - Max(PTSAN_ID_SPACE_BEGIN, 1U));
  initializeIDPool(ClIDBucketSize);
  initialize_stack_id_buckets();
  ptsan_default_id_bounds.base = 0x10000; // filter out Obviously Bad addresses
  ptsan_default_id_bounds.span = 0x0000ffffffffffff;

  // Strict mode never permits accesses through either fallback ID.
  ptsan_bounds[0].base = ClStrictMode ? 0 : 0x10000;
  ptsan_bounds[0].span = ClStrictMode ? 0 : 0x0000ffffffffffff;
#ifdef PTSAN_HAS_DEFAULT_ID
  ptsan_bounds[DefaultIDNo].base = ClStrictMode ? 0 : (u64)DefaultIDNo << 48;
  ptsan_bounds[DefaultIDNo].span = ClStrictMode ? 0 : 0x0000ffffffffffff;
#endif
}

DECLARE_REAL(void *, malloc, SIZE_T size)
DECLARE_REAL(void, free, void *ptr)

extern "C" SANITIZER_INTERFACE_ATTRIBUTE PTSanID
ptsan_get_constant_global_id(void *base, u64 sz, u64 alignment) {
  CHECK(base != nullptr);
  CHECK(sz != 0);

  for (uptr index = 0; index < SharedConstantRangeCount; ++index) {
    const SharedConstantRange &range = SharedConstantRanges[index];
    if (addr_in_range(base, range.id, sz, Polarity::Stripped)) {
#ifdef PTSAN_32_BIT_IDS
      // A region's bias must also preserve this member's alignment.
      if (ptsan_bounds[range.id].base & (alignment - 1)) {
        break;
      }
#endif
      return range.id;
    }
  }

  return ptsan_get_next_global_id(base, sz, alignment);
}

// We should not be making non-writable-according-to-elf memory
// writable or else our global 'just overflow reads' scheme doesn't work
extern "C" SANITIZER_INTERFACE_ATTRIBUTE void
ptsan_check_mprotect(const void *address, u64 size, int protection) {
  if (!(protection & PROT_WRITE) || !size) {
    return;
  }

  // Callers pass the raw address used by the kernel.
  const uptr begin = (uptr)address;
  const uptr end = begin + size;

  for (uptr index = 0; index < SharedConstantRangeCount; ++index) {
    const SharedConstantRange &range = SharedConstantRanges[index];
    if (begin >= range.end || range.begin >= end) {
      continue;
    }

    REPORT_ERROR("PTSan rejected successfully making shared constant memory "
                 "writable: [0x%zx, 0x%zx) overlaps "
                 "[0x%zx, 0x%zx)\n",
                 begin, end, range.begin, range.end);
    internal__exit(1);
  }
}

#ifndef PTSAN_HAS_DEFAULT_ID
static PTSanID get_fallback_id() {
  Printf("PTSan ID32 exhausted its ID space\n");
  internal__exit(1);
}
#else
static PTSanID get_fallback_id() {
  if (ClStrictMode) {
    Printf("PTSan: ID space exhausted in strict mode\n");
    internal__exit(1);
  }
  return DefaultIDNo;
}
#endif

#ifdef PTSAN_VALIDATE
static void record_id_acquired() {
  u64 used = __atomic_fetch_add(&__ptsan_used_ids, 1, __ATOMIC_RELAXED) + 1;
  if (used > __ptsan_max_used_ids) {
    __ptsan_max_used_ids = used;
  }
}

static void validate_stack_id_start(u64 n) {
  u32 bucket = n > 4 ? 4 : n - 1;
  __atomic_fetch_add(ptsan_n_alloca_fn_calls + bucket, 1, __ATOMIC_RELAXED);
}

static void record_stack_id_acquired() {
  record_id_acquired();
  u64 used = __atomic_fetch_add(&__ptsan_used_stack_ids, 1, __ATOMIC_RELAXED) + 1;
  if (used > __ptsan_max_used_stack_ids) {
    __ptsan_max_used_stack_ids = used;
  }
}

static void validate_stack_id_free(PTSanID id) {
  __atomic_fetch_sub(&__ptsan_used_ids, 1, __ATOMIC_RELAXED);
  __atomic_fetch_sub(&__ptsan_used_stack_ids, 1, __ATOMIC_RELAXED);
  CHECK(!is_fast_stack_id(id));
}
#endif

static PTSanID take_stack_id(PTSanThread &thread) {
  PTSanID id = thread.takeID();
  if (!id) {
    return get_fallback_id();
  }

  // TODO(signals): a nonlocal exit between acquisition and publication can
  // leak this ID. The log's stable storage does not close that window.
  ptsan_stack_id_log.push(id);
#ifdef PTSAN_VALIDATE
  record_stack_id_acquired();
#endif

  return id;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_get_stack_ids(PTSanID *ids, u64 n) {
  auto *thread = GetCurrentThread();
#ifdef PTSAN_VALIDATE
  validate_stack_id_start(n);
#endif
  for (u64 i = 0; i != n; ++i) {
    ids[i] = take_stack_id(*thread);
  }
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_free_stack_ids(uptr cursor) {
  auto *thread = GetCurrentThread();
  CHECK_LE(cursor, ptsan_stack_id_log.size());
  while (ptsan_stack_id_log.size() > cursor) {
    PTSanID id = ptsan_stack_id_log.back();
    // Invalidate before relinquishing ownership. A signal-driven nonlocal
    // exit can then either find this ID in the log or leave it already dead.
    if (id) {
      ptsan_bounds[id].span = 0;
      ptsan_obj_types[id] = FREE_TY::NONE;
    }
    // pop is inlined and we don't want to pop before clearing
    // via optimization
    atomic_signal_fence(memory_order_seq_cst);
    ptsan_stack_id_log.pop();
    // A nonlocal exit can leave a reserved slot unpublished.
    if (id) {
#ifdef PTSAN_VALIDATE
      validate_stack_id_free(id);
#endif
      // TODO(signals): A nonlocal exit after the pop can still leak this ID,
      // but its bounds have already been cleared.
      thread->returnID(id);
    }
  }
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_restore_stack_ids(uptr cursor, u64 next_stack_id,
                             PTSanID pending_stack_ids, u8 in_signal_context) {
  // Pop arbitrary IDs from the shared ownership log and recycle them.
  ptsan_free_stack_ids(cursor);

  // Fast IDs occupy a consecutive range in this thread's dedicated bucket.
  // Gaps left by normally returned frames are safe to clear too. The
  // destination frame's IDs are below this range. Always clear on this slow
  // path, even when ordinary fast-path returns may omit temporal cleanup.
  u64 count = ptsan_next_stack_id - next_stack_id;
  // ID32 decoding still needs the bias and origin after retirement.
  for (u64 i = 0; i < count; ++i) {
    ptsan_bounds[next_stack_id + i].span = 0;
  }
  ptsan_pending_stack_ids = pending_stack_ids;
  ptsan_next_stack_id = next_stack_id;
  ptsan_in_signal_context = in_signal_context;
  // Resume instrumented code outside any libc boundary skipped by the jump.
  ptsan_in_libc = 0;
}

static ALWAYS_INLINE void initialize_id(PTSanID id, void *base, u64 sz,
                                       FREE_TY type, u64 alignment,
                                       RandomStream *Random = nullptr) {
  if (isDefaultID(id)) {
    return;
  }
#ifdef PTSAN_VALIDATE
  CHECK(!is_fast_stack_id(id));
  CHECK_EQ(ptsan_bounds[id].span, 0U);
#endif
#ifdef PTSAN_32_BIT_IDS
  write_bounds(id, base, sz,
               choose_id32_payload_start((u64)base, alignment, Random));
#else
  write_bounds(id, base, sz);
#endif
  ptsan_obj_types[id] = type;
}

PTSanID get_id_direct(void *base, u64 sz, FREE_TY type, u64 alignment) {
  PTSanID id = takeIDDirect();
  if (!id) {
    return get_fallback_id();
  }
#ifdef PTSAN_VALIDATE
  record_id_acquired();
#endif
  initialize_id(id, base, sz, type, alignment);
  return id;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
PTSanID ptsan_get_next_global_id(void *base, u64 sz, u64 alignment) {
#ifdef PTSAN_VALIDATE
  __atomic_fetch_add(&__ptsan_used_global_ids, 1, __ATOMIC_RELAXED);
#endif
  PTSanID id = get_id_direct(base, sz, FREE_TY::NONE, alignment);
  assert(!isDefaultID(id));
  return id;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
PTSanID ptsan_get_next_nonstack_id(void *base, u64 sz, FREE_TY type,
                                  u64 alignment) {
  auto *thread = GetCurrentThread();
  PTSanID id = thread->takeID();
  if (!id) {
    id = get_fallback_id();
  }
#ifdef PTSAN_VALIDATE
  record_id_acquired();
#endif
  // TODO(signals): siglongjmp between acquiring and publishing this ID can
  // leak it. Recovery for abandoned operations is separate from this cache.
  initialize_id(id, base, sz, type, alignment, &thread->Random);
  return id;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_resize_buffer(PTSanID id, u64 new_size) {
#ifdef PTSAN_32_BIT_IDS
  if (UNLIKELY(new_size >= (1ULL << 31))) {
    Printf("PTSan ID32 cannot represent objects of 2 GiB or larger\n");
    internal__exit(1);
  }
#endif
  ptsan_bounds[id].span = new_size;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
PTSanID ptsan_get_repeatable_alloca_id(void *base, u64 size, u64 alignment) {
  auto *thread = GetCurrentThread();
  PTSanID id = take_stack_id(*thread);
  initialize_id(id, base, size, FREE_TY::NONE, alignment, &thread->Random);
  return id;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_validate_munmap(const void *address, u64 size) {
  // Callers validate only after Linux accepts the request. Keep this guard for
  // defensive use even though Linux rejects zero-length munmap requests.
  if (size == 0) {
    return;
  }

  const PTSanID id = get_id(address);
  // The fallback ID is the deliberate degradation path after ID exhaustion. It
  // carries no exact object metadata, so preserve normal kernel behavior.
  if (isDefaultID(id)) {
    return;
  }
  // munmap accepts the original byte count, but removes the rounded pages.
  if (id != 0 && ptsan_obj_types[id] == FREE_TY::MMAP &&
      get_object_offset(address, id) == 0 &&
      ptsan_bounds[id].span == RoundUpTo(size, GetPageSizeCached())) {
    return;
  }

  REPORT_ERROR("PTSan rejected successful unsupported munmap: nonempty ranges "
               "must exactly match one tracked mmap allocation\n");
  internal__exit(1);
}

static bool is_exact_mmap(void *address, u64 size) {
  const PTSanID id = get_id(address);
  return ptsan_obj_types[id] == FREE_TY::MMAP &&
         get_object_offset(address, id) == 0 &&
         ptsan_bounds[id].span == RoundUpTo(size, GetPageSizeCached());
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_retire_mmap_if_exact(void *address, u64 size) {
  if (is_exact_mmap(address, size)) {
    ptsan_mark_ptr_freed(address, FREE_TY::MMAP);
  }
}

void ptsan_retire_mmap_if_exact_direct(void *address, u64 size) {
  if (is_exact_mmap(address, size)) {
    PTSanID id = get_id(address);
    ptsan_bounds[id].span = 0;
    ptsan_obj_types[id] = FREE_TY::NONE;
#ifdef PTSAN_VALIDATE
    __atomic_fetch_sub(&__ptsan_used_ids, 1, __ATOMIC_RELAXED);
#endif
    returnIDDirect(id);
  }
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_validate_allocation(const void *ptr, FREE_TY expectedType) {
  const PTSanID id = get_id(ptr);
  if (id == 0) {
    return;
  }
  if (isDefaultID(id)) {
    return;
  }

  u64 current_base = get_object_base(id);
  if (UNLIKELY(get_object_offset(ptr, id) != 0)) {
    REPORT_ERROR(
      "PTsan found invalid (maybe double, maybe something else?) free!. "
      "Base is 0x%llx, addr is 0x%llx, id is 0x%x",
      current_base, (u64)ptr, id
    );
    internal__exit(1);
  }

  if (UNLIKELY(ptsan_obj_types[id] != expectedType)) {
    if (ptsan_obj_types[id] == FREE_TY::NONE && ptsan_bounds[id].span == 0) {
      REPORT_ERROR("PTSan found double-free!\n");
      internal__exit(1);
    }
    REPORT_ERROR("PTSan found bad free of type %d on memory type %d\n", expectedType, ptsan_obj_types[id]);
    internal__exit(1);
  }
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_mark_ptr_freed(void *ptr, FREE_TY freeType) {
  ptsan_validate_allocation(ptr, freeType);
  PTSanID id = get_id(ptr);
  if (id == 0) {
    return;
  }
  if (isDefaultID(id)) {
    #ifdef PTSAN_VALIDATE
      __atomic_fetch_sub(&__ptsan_used_ids, 1, __ATOMIC_RELAXED);
    #endif
    return;
  }

  // Keep the decode bias and origin for stale non-dereference uses until reuse.
  ptsan_bounds[id].span = 0;
  ptsan_obj_types[id] = FREE_TY::NONE;

  GetCurrentThread()->returnID(id);

  #ifdef PTSAN_VALIDATE
  __atomic_fetch_sub(&__ptsan_used_ids, 1, __ATOMIC_RELAXED);
  #endif
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
bool ptsan_check(void *ptr, u64 sz) {
  PTSanID id = get_id(ptr);
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

  u64 base = get_object_base(ptr_id);
#ifndef PTSAN_32_BIT_IDS
  // ID16 checks can report tagged or stripped addresses; ID32 reports raw ones.
  base = get_addr(base);
  min = get_addr(min);
#endif
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
extern "C" SANITIZER_INTERFACE_ATTRIBUTE
PTSanID ptsan_get_id(const void *ptr) {
  return get_id(ptr);
}

#ifndef PTSAN_PRODUCTION_MODE
extern "C" SANITIZER_INTERFACE_ATTRIBUTE
u64 ptsan_get_stack_cursor() {
  return ptsan_next_stack_id;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
u64 ptsan_get_stack_limit() {
  return ptsan_stack_id_limit;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
u64 ptsan_get_address(const void *ptr) {
  return get_addr(ptr);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
u32 ptsan_get_payload(const void *ptr) {
  return (u32)(uptr)ptr;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
u64 ptsan_get_bound(PTSanID id) {
  return get_object_base(id) + ptsan_bounds[id].span;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
u64 ptsan_get_base(PTSanID id) {
  return get_object_base(id);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
u64 ptsan_get_size(PTSanID id) {
  return ptsan_bounds[id].span;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
u32 ptsan_id_bits() {
  return PTSAN_ID_BITS;
}
#endif
