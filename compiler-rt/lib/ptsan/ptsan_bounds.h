//===-- ptsan_bounds.h ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of PointerTagSanitizer.
//
// PTSan object bounds table interface.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "ptsan.h"
#include "ptsan_constants.h"
#include "ptsan_bounds_abi.h"

#ifdef PTSAN_VALIDATE
extern volatile u64 __ptsan_max_used_ids;
extern volatile u64 __ptsan_used_ids;
extern volatile u64 __ptsan_used_global_ids;
extern volatile u64 __ptsan_used_stack_ids;
extern volatile u64 __ptsan_max_used_stack_ids;
#endif

enum FREE_TY : u8 {
  NONE = 0,
  MALLOC = 1,
  MMAP = 2,
};


struct NewRange {
  void *base;
  u64 size;
};

struct StackIDBucket {
  bool avaliable;
  PTSanID start;
  u64 end;
};

extern "C" {

extern SANITIZER_INTERFACE_ATTRIBUTE
THREADLOCAL u64 ptsan_next_stack_id;

extern SANITIZER_INTERFACE_ATTRIBUTE
THREADLOCAL PTSanID ptsan_pending_stack_ids;

// Set while an instrumented application signal handler is active. Setjmp
// checkpoints preserve this so a nonlocal return restores the appropriate ID
// cache.
extern SANITIZER_INTERFACE_ATTRIBUTE
THREADLOCAL u8 ptsan_in_signal_context;

extern SANITIZER_INTERFACE_ATTRIBUTE
THREADLOCAL u64 ptsan_stack_id_limit;

#ifdef PTSAN_32_BIT_IDS
extern SANITIZER_INTERFACE_ATTRIBUTE
ObjectBounds *ptsan_bounds;
extern SANITIZER_INTERFACE_ATTRIBUTE
FREE_TY *ptsan_obj_types;
#else
extern SANITIZER_INTERFACE_ATTRIBUTE
ObjectBounds ptsan_bounds[MetadataNumIDs];
extern SANITIZER_INTERFACE_ATTRIBUTE
FREE_TY ptsan_obj_types[MetadataNumIDs];
#endif

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
ObjectBounds ptsan_default_id_bounds;

extern SANITIZER_INTERFACE_ATTRIBUTE
StackIDBucket *StackIDBuckets;

// Shortcut for our global id case
extern SANITIZER_INTERFACE_ATTRIBUTE
PTSanID ptsan_get_next_global_id(void *base, u64 sz, u64 alignment = 1);

// Give a constant global the permanent ID for its containing non-writable ELF
// mapping. If the object is not wholly contained in a recognized range,
// allocate an ordinary exact-bounds global ID instead.
extern SANITIZER_INTERFACE_ATTRIBUTE PTSanID
ptsan_get_constant_global_id(void *base, u64 sz, u64 alignment = 1);

// Successful permission changes which make a shared constant range writable
// are unsupported: existing pointers cannot be retagged to narrower IDs.
extern SANITIZER_INTERFACE_ATTRIBUTE void
ptsan_check_mprotect(const void *address, u64 size, int protection);

// Acquire IDs and record real (non-default) IDs in the current thread's stack
// log. Bounds are initialized by the caller. The caller must save the log cursor
// before  allocation and pass that cursor to ptsan_free_stack_ids to retire the IDs.
// The caller does it (instead of returning here) as a slight optimization for the
// fast path.
extern SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_get_stack_ids(PTSanID *idsBuf, u64 n);

// Callers supply the allocation's guaranteed alignment, not incidental
// address bits.
extern SANITIZER_INTERFACE_ATTRIBUTE
PTSanID ptsan_get_next_nonstack_id(void *base, u64 sz, FREE_TY type,
                                  u64 alignment);

// Update a live allocation's span after the caller validates its base pointer.
extern SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_resize_buffer(PTSanID id, u64 new_size);

// Explicit runtime-initialization path: no current thread or TLS required.
PTSanID get_id_direct(void *base, u64 sz, FREE_TY type, u64 alignment);

// Acquire an ID and initialize its bounds, using the same stack log and saved
// cursor cleanup as ptsan_get_stack_ids. Default IDs are not logged.
extern SANITIZER_INTERFACE_ATTRIBUTE
PTSanID ptsan_get_repeatable_alloca_id(void *base, u64 size, u64 alignment);

// Retire back to the cursor saved at frame entry or immediately before setjmp.
extern SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_free_stack_ids(uptr cursor);

// Restore ownership and fast-stack state after setjmp's nonzero return, and
// clear in_libc at the instrumented destination. Clear abandoned bounds
// regardless of the fast-path temporal option.
extern SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_restore_stack_ids(uptr cursor, u64 next_stack_id,
                             PTSanID pending_stack_ids, u8 in_signal_context);

extern SANITIZER_INTERFACE_ATTRIBUTE
NewRange ptsan_range_recovery(const void *ptr, u64 sz);

extern SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_mark_ptr_freed(void *ptr, FREE_TY freeType);

// Validate a tracked allocation's base and type without retiring its ID.
extern SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_validate_allocation(const void *ptr, FREE_TY expectedType);

extern SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_validate_munmap(const void *address, u64 size);

// Retire a live MMAP allocation only when address and page-rounded size
// describe it exactly. Unknown and unsupported ranges are ignored.
extern SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_retire_mmap_if_exact(void *address, u64 size);

// Final thread-exit path: same exact-range check, but return the ID directly
// to the shared pool after the thread/cache has been destroyed. The caller
// must have called FinishMuslThread(), which leaves signals blocked through
// the final unmap/exit so no handler runs without a PTSan thread.
void ptsan_retire_mmap_if_exact_direct(void *address, u64 size);

extern SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_complain(u32 check_id, u64 function_id, u64 ptr_id, const char *type, u64 min, u64 max);

extern SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_complain_vector(u32 check_id, u64 function_id);

} // Extern C

SANITIZER_INTERFACE_ATTRIBUTE
extern int ClValidate;

SANITIZER_INTERFACE_ATTRIBUTE
extern int ClQuietErrors;

SANITIZER_INTERFACE_ATTRIBUTE
extern int ClAbortOnError;

#define REPORT_ERROR(...)                                                      \
  if (!ClQuietErrors) {                                                        \
    Printf(__VA_ARGS__);                                                       \
  }
