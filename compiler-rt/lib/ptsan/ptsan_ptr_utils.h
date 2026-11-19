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
#include "ptsan_origins.h"
#include "ptsan_musl_interception.h"

#include <stddef.h>

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

// get_addr and clear_id share ptsan_raw_address's decoding contract. In strict
// ID32, neither may be applied to an already-decoded address.
template <typename T>
inline u64 get_addr(T ptr) {
  return ptsan_raw_address((uintptr_t)ptr);
}

template <typename T>
inline T clear_id(T ptr) {
    return (T)get_addr(ptr);
}

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

#ifdef PTSAN_32_BIT_IDS
template <typename T>
inline PTSanID get_id(T ptr) {
  u64 bits = (u64)ptr;
#ifndef PTSAN_STRICT_32_BIT_IDS
  if (!ptsan_is_encoded_id32(bits)) {
    return 0;
  }
#endif
  return (PTSanID)(bits >> 32);
}
#else
template <typename T>
inline PTSanID get_id(T ptr) {
  return (PTSanID)((u64)ptr >> 48);
}
#endif

#ifdef PTSAN_32_BIT_IDS
template <typename T>
inline T set_id(T ptr, PTSanID id) {
  // In strict ID32 mode tagging consumes a known raw address, not an unknown
  // mixture of raw and encoded pointers.
#ifdef PTSAN_STRICT_32_BIT_IDS
  u64 raw = (u64)ptr;
#else
  u64 raw = get_addr(ptr);
#endif
  if (id == 0) {
    return (T)raw;
  }
  u64 payload = raw - ptsan_bounds[id].base;
  CHECK(payload <= UINT32_MAX);
  u64 tagged = ((u64)id << 32) | (u32)payload;
  return (T)tagged;
}
#else
template <typename T>
inline T set_id(T ptr, PTSanID id) {
  u64 raw = ((u64)id << 48) | get_addr(ptr);
  return (T)raw;
}
#endif

// Return the object base in the representation used for bounds checks:
// tagged in ID16, raw in ID32. ID32 decoding instead uses the stored bias.
#ifdef PTSAN_32_BIT_IDS
inline u64 get_object_base(PTSanID id) {
  return ptsan_bounds[id].base + ptsan_origins[id];
}
#else
inline u64 get_object_base(PTSanID id) {
  return ptsan_bounds[id].base;
}
#endif

enum class Polarity { Tagged, Stripped };

// Polarity describes the caller's representation, not whether its ID is nonzero.
#ifdef PTSAN_32_BIT_IDS
template <typename T>
inline u64 get_object_offset(T ptr, PTSanID id,
                             Polarity polarity = Polarity::Tagged) {
  u64 address = (u64)ptr;
  if (polarity == Polarity::Tagged) {
    address = get_addr(ptr);
  }
  return address - get_object_base(id);
}
#else
template <typename T>
inline u64 get_object_offset(T ptr, PTSanID id,
                             Polarity polarity = Polarity::Tagged) {
  u64 base = get_object_base(id);
  if (polarity == Polarity::Stripped)
    base = get_addr(base);
  return (u64)ptr - base;
}
#endif

template<typename T>
inline bool addr_in_range(T ptr, PTSanID id, u64 size,
                          Polarity polarity = Polarity::Tagged) {
  u64 span  = ptsan_bounds[id].span;

  if (size > span) {
    return false;
  }

  u64 offset = get_object_offset(ptr, id, polarity);
  u64 slack = span - size;
  return offset <= slack;
}

// False if bad range
template<typename T>
inline bool check_range(T ptr, u64 size) {
  PTSanID id = get_id(ptr);
  // size == 0 accesses are always valid
  return addr_in_range(ptr, id, size) || size == 0;
}

// Return the number of accessible bytes from ptr to the end of its object.
template <typename T>
inline u64 bytes_remaining(T ptr) {
  PTSanID id = get_id(ptr);
  const auto &bounds = ptsan_bounds[id];
  const u64 offset = get_object_offset(ptr, id);
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

// Encode dest using src's ID. In strict ID32, non-null dest must be a raw
// address belonging to src's object, not an already-encoded pointer.
template <typename DstT, typename SrcT>
inline DstT copy_id(DstT dest, SrcT src) {
  if (dest == nullptr) {
    return dest;
  }

  return set_id(dest, get_id(src));
}

DECLARE_REAL(SIZE_T, malloc_usable_size, void *ptr)

/** Tag a pointer with the next id
 *
 * You need to then reassign the pointer
 *
 * THIS IS A NOP IF ptsan_in_libc IS > 0
**/
template <typename T>
inline T tag_with_next_id(T ptr, u64 size, FREE_TY type, u64 alignment) {
  // We never tag ptrs from libc internals, since
  // those same internals often dereference them
  // which explodes. Call this once you've exited
  // libc if you want to assign a ptr id
  if(ptsan_in_libc) {
    return ptr;
  }

#ifndef PTSAN_STRICT_32_BIT_IDS
  CHECK(get_id(ptr) == 0);
#endif

  if(UNLIKELY(ClUseReturnedMallocSize && type == FREE_TY::MALLOC)) {
    size = REAL(malloc_usable_size)(ptr);
  }

  PTSanID id = ptsan_get_next_nonstack_id((void*)ptr, size, type, alignment);

  return set_id(ptr, id);
}

// Resize the buffer *which clear_id(ptr) points to the
// start of*.
//
// Nop if ptr is untagged
template <typename T>
inline void resize_buffer(T ptr, u64 new_size) {
  PTSanID id = get_id(ptr);
  u64 addr = get_addr(ptr);

  if (id == 0 || isDefaultID(id)) {
    return;
  }

  CHECK_EQ(get_object_offset(ptr, id), 0);

  if(UNLIKELY(ClUseReturnedMallocSize &&
     ptsan_obj_types[id] == FREE_TY::MALLOC)) {
    new_size = REAL(malloc_usable_size)((void*)addr);
  }

  ptsan_resize_buffer(id, new_size);
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
    return tag_with_next_id(new_ptr, new_size, FREE_TY::MALLOC,
                            alignof(max_align_t));
  }

  // If old_ptr == new_ptr, it's still valid, so we
  // do *not* want to change the ID, just resize it
  if (new_ptr == clear_id(old_ptr)) {
    resize_buffer(old_ptr, new_size);
    return old_ptr;
  }

  // If we get here, old_ptr was freed and we need a new id:
  ptsan_mark_ptr_freed((void *)old_ptr, FREE_TY::MALLOC);

  return tag_with_next_id(new_ptr, new_size, FREE_TY::MALLOC,
                          alignof(max_align_t));
}
