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


struct ObjectBounds {
  u64 base;
  u64 span;
};

struct NewRange {
  void *base;
  u64 size;
};

struct StackIDBucket {
  bool avaliable;
  u16 start;
  u16 end;
};

extern "C" {

extern SANITIZER_INTERFACE_ATTRIBUTE
THREADLOCAL u16 ptsan_next_stack_id;

extern SANITIZER_INTERFACE_ATTRIBUTE
THREADLOCAL u16 ptsan_pending_stack_ids;

extern SANITIZER_INTERFACE_ATTRIBUTE
THREADLOCAL u16 ptsan_stack_id_limit;

extern SANITIZER_INTERFACE_ATTRIBUTE
ObjectBounds ptsan_bounds[MetadataNumIDs];

extern SANITIZER_INTERFACE_ATTRIBUTE
FREE_TY ptsan_obj_types[MetadataNumIDs];

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
ObjectBounds ptsan_default_id_bounds;

extern SANITIZER_INTERFACE_ATTRIBUTE
u32 ptsan_active_stack_id_buckets;

extern SANITIZER_INTERFACE_ATTRIBUTE
StackIDBucket StackIDBuckets[MAX_STACK_ID_BUCKETS];

// Shortcut for our global id case
extern SANITIZER_INTERFACE_ATTRIBUTE
u16 ptsan_get_next_global_id(void *base,  u64 sz);

extern SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_get_stack_ids(u16 *idsBuf, u64 n);

extern SANITIZER_INTERFACE_ATTRIBUTE
u16 ptsan_get_next_nonstack_id(void *base, u64 sz, FREE_TY type);

extern SANITIZER_INTERFACE_ATTRIBUTE
NewRange ptsan_range_recovery(const void *ptr, u64 sz);

// Given 12 -> 14 -> 15 -> 66, sets LastID->next = 12, and sets
// LastID to 66
extern SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_free_stack_ids(u16 *ids, u64 n);

extern SANITIZER_INTERFACE_ATTRIBUTE
bool ptsan_mark_ptr_freed(void *ptr, FREE_TY freeType);

extern SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_complain(u32 check_id, u64 function_id, u64 ptr_id, const char *type, u64 min, u64 max);

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
