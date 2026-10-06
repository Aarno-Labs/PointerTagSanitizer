//===-- ptsan_ptr_utils.h ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of PointerTagSanitizer.
//
// Inline helpers for PTSan pointer ids.
//
//===----------------------------------------------------------------------===//
// Header file so it inlines properly
#pragma once

#include "ptsan.h"
#include "ptsan_constants.h"
#include "ptsan_bounds.h"
#include "ptsan_musl_interception.h"

template<typename T> struct is_pointer { static constexpr bool value = false; };
template<typename T> struct is_pointer<T*> { static constexpr bool value = true; };

template <typename T> struct is_void { static constexpr bool value = false; };
template <>struct is_void<void> { static constexpr bool value = true; };


#define ENSURE_PTSAN_INITED() do { \
  CHECK(!ptsan_init_is_running); \
  if (!ptsan_initialized) { \
    __ptsan_init(); \
  } \
} while (0)

extern SANITIZER_INTERFACE_ATTRIBUTE THREADLOCAL u32 ptsan_in_libc;

template <typename T>
inline u64 get_addr(T ptr) {
    return ((u64)ptr & 0x0000ffffffffffff);
}

#ifdef PTSAN_NO_STRIP
  #define clear_id(ptr) ptr
#else
template <typename T>
inline T clear_id(T ptr) {
    return (T)get_addr(ptr);
}
#endif

#ifdef PTSAN_NO_STRIP
  #define clear_id_preserve_align(ptr) ptr
#else
template <typename T>
inline T clear_id_preserve_align(T ptr)
{
    /* 1. strip tag bits */
    u64 raw = get_addr(ptr);

    /* 2. figure out natural alignment of the POINTED-TO type */
    typedef __typeof__(*ptr) pointee_t;      /* e.g. __pthread_unwind_buf_t */
    const u64 A = __alignof__(pointee_t); /* 16 for unwind buffer        */

    /* 3. tell the optimiser the pointer still meets that alignment */
    return (T)__builtin_assume_aligned((void *)raw, A);
}
#endif

// Strip IDs off of an unknown value if-and-only-if it's a pointer. If you
// know for sure you're dealing with a pointer, I guess use clear_id from
// above, although these should (should!) compile to the same things
template <class T>
ALWAYS_INLINE T stripID(T val) {
  if constexpr (is_pointer<T>::value) {
    // Do not strip ids on -1!
    if (val == (T)-1) {
      return val;
    }
    return clear_id(val);
  } else {
    return val;
  }
}

/* Doesn't work -- needs compile-time reflection. I miss scheme...
template <class T>
ALWAYS_INLINE T *copyStruct(T &out, T &in) {
  for (constexpr field in in) {
    if constexpr (is_pointer_to_struct<field_type>::value) {
      out->field = deepStripID(in.field);
    } else if constexpr (is_pointer<field_type>::value) {
      out->field = clear_id(in.field)
    } else if constexpr (is_struct<field_type>::value) {
      copyStruct(out, in);
    } else {
      out->field = in.field;
    }
  }
}

template <class T>
ALWAYS_INLINE T *deepStripID(T *in) {
  T *out = (T)malloc(sizeof(T));
  copyStruct(*out, *in);
  return T
}*/

template <typename T>
inline u16 get_id(T ptr) {
    return (u16)((u64)ptr >> 48);
}

template <typename T>
inline T set_id(T ptr, u16 id) {
    u64 raw = ((u64)id << 48) | get_addr(ptr);
    return (T)raw;
}

template<typename T>
inline bool addr_in_range(T ptr, u16 id, u64 size) {
  u64 base  = ptsan_bounds[id].base;
  u64 span  = ptsan_bounds[id].span;

  if (size > span) {
    return false;
  }

  u64 offset = (u64)ptr - base;
  u64 slack = span - size;
  return offset <= slack;
}

// False if bad range
template<typename T>
inline bool check_range(T ptr, u64 size) {
  u16 id = get_id(ptr);
  // size == 0 accesses are always valid
  return addr_in_range(ptr, id, size) || size == 0;
}

// Use the tagged address, just as addr_in_range does: bounds bases include
// their ID. Reserved IDs are governed by their table entries too, including
// the empty bounds installed when strict-mode initialization finishes.
template <typename T>
inline u64 bytes_remaining(T ptr) {
  const auto &bounds = ptsan_bounds[get_id(ptr)];
  const u64 offset = (u64)ptr - bounds.base;
  return offset <= bounds.span ? bounds.span - offset : 0;
}

inline void report_scan_read(const void *ptr, u64 index) {
  ptsan_complain(0, 0xdeadbeef, get_id(ptr), "read", get_addr(ptr) + index, 1);
}

// Return the first matching byte's offset, or size if none matches. A match
// before the bounds end is valid even when size extends beyond it;
// size == 0 never reads or reports.
// Bounds are loaded once; report before the first invalid read. If reporting
// returns, continue with the ordinary operation, as other interceptors do.
// The predicate can also express set membership for strpbrk/strcspn, or
// non-membership for strspn, without duplicating the bounds-aware scan.
template <typename Matches>
inline u64 find_matching_byte(const void *source, u64 size, Matches matches) {
  const auto *raw = (const unsigned char *)clear_id(source);
  const u64 available = bytes_remaining(source);
  for (u64 index = 0; index < size; ++index) {
    if (index == available) {
      report_scan_read(source, index);
    }
    if (matches(raw[index])) {
      return index;
    }
  }
  return size;
}

// Stop at the target or NUL, whichever comes first.
inline u64 find_cstring_character(const char *source, int character) {
  const unsigned char target = (unsigned char)character;
  return find_matching_byte(source, UINT64_MAX, [target](unsigned char value) {
    return value == target || value == '\0';
  });
}

inline u64 find_bounded_character(const void *source, int character, u64 size) {
  const unsigned char target = (unsigned char)character;
  return find_matching_byte(source, size, [target](unsigned char value) {
    return value == target;
  });
}

template <typename T>
inline bool check_range_and_report(u32 check_id, T ptr, u64 size,
                                   const char *action, u64 extra_space = 0) {
  if (!check_range(ptr, size + extra_space)) {
    // String callers check an additional terminator byte, but diagnostics
    // retain the caller's original size, as in the existing interceptors.
    ptsan_complain(check_id, 0xdeadbeef, get_id(ptr), action, get_addr(ptr), size);
    return false;
  }
  return true;
}

template <typename DstT, typename SrcT>
inline DstT copy_id(DstT dest, SrcT src) {
  if (dest == nullptr) {
    return dest;
  }

  u64 combined = ((u64)src & 0xffff000000000000) | (u64)dest;
  return (DstT)combined;
}

DECLARE_REAL(SIZE_T, malloc_usable_size, void *ptr)

/** Tag a pointer with the next id
 *
 * You need to then reassign the pointer
 *
 * THIS IS A NOP IF ptsan_in_libc IS > 0
**/
template <typename T>
inline T tag_with_next_id(T ptr, u64 size, FREE_TY type) {
  // We never tag ptrs from libc internals, since
  // those same internals often dereference them
  // which explodes. Call this once you've exited
  // libc if you want to assign a ptr id
  if(ptsan_in_libc) {
    return ptr;
  }

  CHECK(get_id(ptr) == 0);

  if(UNLIKELY(ClUseReturnedMallocSize && type == FREE_TY::MALLOC)) {
    size = REAL(malloc_usable_size)(ptr);
  }

  u16 id = ptsan_get_next_nonstack_id((void*)ptr, size, type);

  return set_id(ptr, id);
}

// Resize the buffer *which clear_id(ptr) points to the
// start of*.
//
// Nop if ptr is untagged
template <typename T>
inline void resize_buffer(T ptr, u64 new_size) {
  u16 id = get_id(ptr);
  u64 addr = get_addr(ptr);

  if (id == 0 || id == DefaultIDNo) {
    return;
  }

  CHECK(ptsan_bounds[id].base == (u64)ptr);

  if(UNLIKELY(ClUseReturnedMallocSize &&
     ptsan_obj_types[id] == FREE_TY::MALLOC)) {
    new_size = REAL(malloc_usable_size)((void*)addr);
  }

  ptsan_bounds[id].span = new_size;
}

// new_ptr is the raw, untagged allocator result (possibly null).
// old_ptr retains the caller's original pointer and ID (possibly null).
template <typename T>
T *tag_realloc(T *new_ptr, T* old_ptr, SIZE_T new_size) {
  // If realloc failed, nothing changed memory-wise:
  if (new_ptr == nullptr) {
    return new_ptr;
  }

  // If old_ptr was null, this is just a malloc:
  if (old_ptr == nullptr) {
    return tag_with_next_id(new_ptr, new_size, FREE_TY::MALLOC);
  }

  // If old_ptr == new_ptr, it's still valid, so we
  // do *not* want to change the ID, just resize it
  if (new_ptr == clear_id(old_ptr)) {
    resize_buffer(old_ptr, new_size);
    return old_ptr;
  }

  // If we get here, old_ptr was freed and we need a new id:
  if(!ptsan_mark_ptr_freed((void*)old_ptr, FREE_TY::MALLOC)) {
    Printf("PTSan found bad realloc. Deal with this\n");
    internal__exit(1);
  }

  return tag_with_next_id(new_ptr, new_size, FREE_TY::MALLOC);
}
