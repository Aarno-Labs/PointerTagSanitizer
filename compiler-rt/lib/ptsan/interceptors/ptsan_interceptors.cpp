//===-- ptsan_interceptors.cpp ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of PointerTagSanitizer
//
// Interceptors for libc
//
// At a high level, the problem is simple: each libc wrapper will (potentially)
// take tagged pointers. We need to remove the IDs before calling into code we
// don't control, and (ideally) add them back (or make new ones) for returned
// pointers.
//
// Many of these wrappers we write ourselves, but llvm has a common_interceptors.inc
// include file which contains ~800+ libc function interceptors with hooks for
// asan/msan/etc.
//
// Unfortunately, those interceptors are not quite what we need, so we have
// taken them and edited them to make ptsan_common_interceptors.inc.
//
// The constraints we are dealing with are something like:
//
// - Each wrapper will call `REAL(func)` somewhere in it, which delegates to
//   real libc
// - A wrapper may call multiple `REAL(func)`s
// - Each wrapper has a number of macro hooks, which can be used to check
//   accesses (COMMON_INTERCEPTOR_WRITE_RANGE) to check a block write, for instance
//   - These macros are already in the right position to do all our libc checking
//     if we just define them right
// - Some REAL functions return newly allocated memory that we (ideally) want to
//   give an id to. Others return pointers to existing memory which is either:
//   - a.) identical to one of its arguments (strcpy returns dest) or
//   - b.) a pointer into one of its arguments (strstr points to the src buffer, but
//         not at the passed-in pointer)
// - Wrappers can do real work -- sometimes they do pointer arithmetic or partially
//   reimplement part of libc and call into different primatives than the
//   function they're implementing
// - The `REAL(func)` calls all need to have IDs stripped out of their arguments.
// - Each wrapper has a `COMMON_INTERCEPTOR_ENTER` macro, which could be used
//   to strip out ids from all arguments, but:
//   - If we strip the ids out at the top, we can't check anything in the check macros.
//   - One could get clever with redefining a ptr as `ptr_id = get_id(ptr);
//     ptr = stripID(ptr)`, but this doesn't handle the case where pointer arithmetic
//     creates a new pointer based off of an existing one:
// - Because there is pointer arithmetic in some wrappers which produces pointers
//   which must be checked, we need to leave IDs in the pointers until they
//   are actually passed off to libc
// - The wrappers can contain pointer dereferences. Those dereferences need to
//   have their IDs stripped.
//
// Thus, given all of those constraints we end up with the following scheme for
// llvm-provided libc wrappers. (Note that for our own wrappers we can just
// write them to avoid these problems, but I don't want to rewrite 800 libc
// wrappers if I don't have to):
// - Pointers are passed in to each wrapper (and pass through COMMON_INTERCEPTOR_ENTER)
//   unmodified
// - REAL(func) is overwritten to point to `__ptsan_func`. This is a wrapper
//   on REAL(func) which:
//   - increments ptsan_in_libc on call, and decrements on return
//     - When this is set, malloc (and others) won't return pointers with ids,
//       which would otherwise break libc functions which allocate-and-then-use
//       pointers.
//   - Removes the ids from each argument which is a pointer
//   - On return, looks at the return value, and looks at each argument pointer.
//     If the return is into any of the arguments buffers, it copies the argument
//     pointer's id to the return value
// - We run PTSanIDStrip on the body of each interceptor. All that pass does is
//   remove the id before each pointer dereference.
//
// This is *far* from perfect. Current holes are:
// - Pointers to structs which themselves contain pointers are not recursed
//   on. If libc accesses a pointer in those struct, they'll explode.
// - Any libc function which allocates new memory has to be special-cased.
//   - This just means we drop taint, not that they'll explode, thankfully
// - Iterating and comparing the return to args is inefficient -- we would
//   cut overhead if we just manually copied IDs over
// - This doesn't handle varag functions at all. As far as I can tell, it's
//   impossible to do this in a general way. For printf/scanf family, we have
//   a seperate implementation which handles their vararg in
//   ptsan_interceptors_format.cpp
//
// And of course there area bunch of libc functions not included in the llvm
// libc wrappers. They are added here on an as-needed basis
//===----------------------------------------------------------------------===//

#define SANITIZER_COMMON_NO_REDEFINE_BUILTINS

#include "../ptsan.h"
#include "ptsan_interceptors.h"
#include "../ptsan_bounds.h"
#include "../ptsan_thread.h"
#include "../ptsan_ptr_utils.h"
#include "../ptsan_drift.h"

#include "interception/interception.h"

#include <stdarg.h>
#include <sys/syscall.h>

#include "sanitizer_common/sanitizer_linux.h"
#include "sanitizer_common/sanitizer_type_traits.h"

using namespace __sanitizer;
using namespace __ptsan;

#if !SANITIZER_LINUX
  #error "PTSan only works on linux
#endif

SANITIZER_INTERFACE_ATTRIBUTE THREADLOCAL u32 ptsan_in_libc = 0;

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_enable_libc_instr() {
  ptsan_in_libc = 0;
}

// Internal libc helpers that handle PTSan-tagged pointers.
#include "ptsan_libc.inc"

// Given out (the return value of a function) and args (the arguments to said function),
// and:
// - If out is not a pointer, don't do anything
// - If out is a pointer, iterate the arguments. Skip
//   non-pointer arguments. If any of the arguments = out,
//   set out.id = arg.id
template <class T>
T addRetID(T out)
{
  //CHECK(false && "Pointer returned which did not match inputs. Special-case this function!");
  return out;
}
template <class T, class FirstArg, class... Rest>
T addRetID(T out, FirstArg first, Rest... rest) {
  if constexpr (!is_pointer<T>::value) {
    return out;
  } else {
    if (out == nullptr) {
      return out;
    }

    if constexpr (is_pointer<FirstArg>::value) {
      u16 id = get_id(first);
      T tagged_out = set_id(out, id);
      if (id && addr_in_range(tagged_out, id, 1)) {
        return tagged_out;
      }
    }

    return addRetID(out, rest...);
  }
}

// The meat of our wrapper. It:
// - Increments/decrements in-libc. While in-libc > 0, malloc and other id-spawning
//   function won't add ids (because sometimes those pointers are used and things)
//   explode. If an intercepted function allocates fresh memory in a REAL call,
//   you should handle it separately.
// - Strips IDs from all entering pointers
//   - Note that it does not recurse structs. Those functions need to be handled
//     separately if indeed they will dereference contained pointers.
//     (XXX: Template magic to walk structs???)
// - Re-adds the ID if the return value is one of the args. This is a very common
//   pattern, so we handle it by default here
// - Gracefully handles the case where f is void, which was surprisingly complicated
template <typename F, typename... Args> auto doPTSanCall(F f, Args... args) {
  // 'Ret' is "whatever f(...) returns"
  using RetTy = decltype(f(args...));

  u32 old = ptsan_in_libc++;
  if constexpr (is_void<RetTy>::value) {
    f(stripID(args)...);
    CHECK(ptsan_in_libc > 0);
    ptsan_in_libc = old;
  } else {
    auto out = f(stripID(args)...);
    CHECK(ptsan_in_libc > 0);
    ptsan_in_libc = old;
    return addRetID(out, args...);
  }
}


#define check_and_report_range_norec(ptr, size, action) \
  check_range_and_report(__COUNTER__, ptr, size, action)

#define check_and_report_range(ptr, size, action) \
  check_range_and_report(__COUNTER__, ptr, size, action)

#define check_and_report_range_str(ptr, size, action) \
  check_range_and_report(__COUNTER__, ptr, size, action, 1)

////////////////////////////////
// PTSAN-SPECIFIC INTERCEPTORS
///////////////////////////////

INTERCEPTOR(void, free, void *ptr) {
  if (UNLIKELY(ptsan_init_is_running)) {
    CHECK(get_id(ptr) == 0);
    return REAL(free)(ptr);
  }

  ENSURE_PTSAN_INITED();

  if(ptsan_mark_ptr_freed(ptr, FREE_TY::MALLOC)) {
    return REAL(free)(clear_id(ptr));
  }
}

INTERCEPTOR(void *, malloc, SIZE_T size) {
  if(UNLIKELY(ptsan_init_is_running)) {
    return REAL(malloc)(size);
  }

  ENSURE_PTSAN_INITED();

  void *out = REAL(malloc)(size);
  if (out == nullptr) {
    return nullptr;
  }

  return tag_with_next_id(out, size, FREE_TY::MALLOC);
}

INTERCEPTOR(int, posix_memalign, void **memptr, SIZE_T alignment, SIZE_T size) {
  // Like the other allocators: skip tagging while ptsan is still initializing.
  if (UNLIKELY(ptsan_init_is_running)) {
    return REAL(posix_memalign)(memptr, alignment, size);
  }

  ENSURE_PTSAN_INITED();

  int res = REAL(posix_memalign)(clear_id(memptr), alignment, size);
  if (res != 0) {
    return res;
  }
  *memptr = tag_with_next_id(*memptr, size, FREE_TY::MALLOC);
  return res;
}

INTERCEPTOR(void *, calloc, SIZE_T nmemb, SIZE_T size) {
  if (UNLIKELY(ptsan_init_is_running))
    return REAL(calloc)(nmemb, size);

  ENSURE_PTSAN_INITED();

  void *out = REAL(calloc)(nmemb, size);
  if (out == nullptr) {
    return nullptr;
  }

  SIZE_T tot = nmemb * size;
  return tag_with_next_id(out, tot, FREE_TY::MALLOC);
}

static inline bool validate_realloc(void *ptr) {
  u16 id = get_id(ptr);
  if(id == 0) {
    return true;
  }

  if (UNLIKELY(ptsan_obj_types[id]) != FREE_TY::MALLOC) {
    return false;
  }

  return true;
}

INTERCEPTOR(void *, realloc, void *ptr, SIZE_T size) {
  if (UNLIKELY(ptsan_init_is_running)) {
    CHECK(get_id(ptr) == 0);
    return REAL(realloc)(ptr, size);
  }

  ENSURE_PTSAN_INITED();

  void *real_ptr = clear_id(ptr);
  void *out = REAL(realloc)(real_ptr, size);
  return tag_realloc(out, ptr, size);
}

DECLARE_REAL(void *, memcpy, void *dest, const void *src, uptr n)
DECLARE_REAL(void *, memset, void *dest, int c, uptr n)
DECLARE_REAL(void *, memmove, void *dest, const void *src, uptr n)

// Actual interception happens in sanitizer_common_interceptors_memintrinsics.inc
void *__ptsan_memcpy(void *dest, const void *src, SIZE_T n) {
  if (!ptsan_initialized) return internal_memcpy(dest, src, n);
  if (ptsan_init_is_running) return REAL(memcpy)(dest, src, n);

  check_and_report_range(dest, n, "write");
  check_and_report_range(src, n, "read");

  void *res = REAL(memcpy)(clear_id(dest), clear_id(src), n);
  CHECK(res == clear_id(dest));

  return dest;
}

void *__ptsan_memset(void *s, int c, SIZE_T n) {
  if (!ptsan_initialized) return internal_memset(s, c, n);
  if (ptsan_init_is_running) return REAL(memset)(s, c, n);

  check_and_report_range(s, n, "write");
  void *res = REAL(memset)(clear_id(s), c, n);
  CHECK(res == clear_id(s));

  return s;
}

void *__ptsan_memmove(void *dest, const void *src, SIZE_T n) {
  if (!ptsan_initialized) return internal_memmove(dest, src, n);
  if (ptsan_init_is_running) return REAL(memmove)(dest, src, n);

  check_and_report_range(dest, n, "write");
  check_and_report_range(src, n, "read");

  void *res = REAL(memmove)(clear_id(dest), clear_id(src), n);
  CHECK(res == clear_id(dest));

  return dest;
}

INTERCEPTOR(void *, mempcpy, void *dest, const void *src, SIZE_T n) {
  __ptsan_memcpy(dest, src, n);
  return (char*)dest + n;
}

// Keep these C ABI wrappers out of the file that includes <wchar.h>: glibc
// exposes conflicting const-preserving C++ overloads when compiled with GCC.
INTERCEPTOR(wchar_t *, wmemchr, const wchar_t *s, wchar_t c, SIZE_T n) {
  ENSURE_PTSAN_INITED();
  InLibcGuard _ig;
  wchar_t *res = REAL(wmemchr)(clear_id(s), c, n);
  return copy_id(res, s);
}

INTERCEPTOR(wchar_t *, wcschr, const wchar_t *s, wchar_t c) {
  ENSURE_PTSAN_INITED();
  InLibcGuard _ig;
  wchar_t *res = REAL(wcschr)(clear_id(s), c);
  return copy_id(res, s);
}

INTERCEPTOR(wchar_t *, wcsrchr, const wchar_t *s, wchar_t c) {
  ENSURE_PTSAN_INITED();
  InLibcGuard _ig;
  wchar_t *res = REAL(wcsrchr)(clear_id(s), c);
  return copy_id(res, s);
}

INTERCEPTOR(wchar_t *, wcsstr, const wchar_t *haystack, const wchar_t *needle) {
  ENSURE_PTSAN_INITED();
  InLibcGuard _ig;
  wchar_t *res = REAL(wcsstr)(clear_id(haystack), clear_id(needle));
  return copy_id(res, haystack);
}

INTERCEPTOR(wchar_t *, wcspbrk, const wchar_t *s, const wchar_t *accept) {
  ENSURE_PTSAN_INITED();
  InLibcGuard _ig;
  wchar_t *res = REAL(wcspbrk)(clear_id(s), clear_id(accept));
  return copy_id(res, s);
}

#define GET_STORE_STACK_TRACE // Pass -- someday we'll probably want this


INTERCEPTOR(char *, strncpy, char *dest, const char *src, SIZE_T n) {
  ENSURE_PTSAN_INITED();
  GET_STORE_STACK_TRACE;
  find_bounded_character(src, '\0', n);
  // strncpy pads the destination through n, even after an early NUL.
  check_and_report_range(dest, n, "write");

  InLibcGuard _ig;
  char *res = REAL(strncpy)(clear_id(dest), clear_id(src), n);

  CHECK(res == clear_id(dest));
  return dest;
}

INTERCEPTOR(char *, strcpy, char *dest, const char *src) {
  ENSURE_PTSAN_INITED();
  GET_STORE_STACK_TRACE;
  SIZE_T n = find_cstring_character(src, '\0');

  // the _str version adds +1 for the null terminator
  if (!check_and_report_range_str(dest, n, "write")) {
    InLibcGuard _ig;
    char *res = REAL(strncpy(clear_id(dest), clear_id(src), n));
    res[n] = '\0';
    CHECK(res == clear_id(dest));
    return dest;
  }

  InLibcGuard _ig;
  char *res = REAL(strcpy)(clear_id(dest), clear_id(src));

  CHECK(res == clear_id(dest));
  return dest;
}

INTERCEPTOR(char *, stpncpy, char *dest, const char *src, SIZE_T n) {
  ENSURE_PTSAN_INITED();
  GET_STORE_STACK_TRACE;
  find_bounded_character(src, '\0', n);
  check_and_report_range(dest, n, "write");

  InLibcGuard _ig;
  char *res = REAL(stpncpy)(clear_id(dest), clear_id(src), n);

  return copy_id(res, dest);
}

INTERCEPTOR(char *, stpcpy, char *dest, const char *src) {
  ENSURE_PTSAN_INITED();
  GET_STORE_STACK_TRACE;
  SIZE_T n = find_cstring_character(src, '\0');
  // the _str version adds +1 for the null terminator
  if (!check_and_report_range_str(dest, n, "write")) {
    InLibcGuard _ig;
    char *res = REAL(stpncpy(clear_id(dest), clear_id(src), n));
    res[n] = '\0';
    CHECK(res == clear_id(dest));
    return dest;
  }

  InLibcGuard _ig;
  char *res = REAL(stpcpy)(clear_id(dest), clear_id(src));

  return copy_id(res, dest);
}


INTERCEPTOR(char *, strdup, char *src) {
  ENSURE_PTSAN_INITED();
  GET_STORE_STACK_TRACE;
  SIZE_T n = find_cstring_character(src, '\0');

  // guard only the libc call: tag_with_next_id is a NOP while ptsan_in_libc > 0
  char *res;
  {
    InLibcGuard _ig;
    res = REAL(strdup)(clear_id(src));
  }

  // n+1 for null terminator
  res = tag_with_next_id(res, n + 1, FREE_TY::MALLOC);
  return res;
}

INTERCEPTOR(char *, __strdup, char *src) {
  ENSURE_PTSAN_INITED();
  GET_STORE_STACK_TRACE;
  SIZE_T n = find_cstring_character(src, '\0');

  char *res;
  {
    InLibcGuard _ig;
    res = REAL(__strdup)(clear_id(src));
  }

  // n+1 for null terminator
  res = tag_with_next_id(res, n + 1, FREE_TY::MALLOC);
  return res;
}

INTERCEPTOR(char *, gcvt, double number, SIZE_T ndigit, char *buf) {
  ENSURE_PTSAN_INITED();

  // XXX: Really should put the check above the call, but that's hard
  char *res;
  {
    InLibcGuard _ig;
    res = REAL(gcvt)(number, ndigit, clear_id(buf));
  }

  SIZE_T n = internal_strlen(buf);
  check_and_report_range_norec(buf, n + 1, "write");
  return res;
}

INTERCEPTOR(char *, strcat, char *dest, const char *src) {
  ENSURE_PTSAN_INITED();
  GET_STORE_STACK_TRACE;
  SIZE_T src_size = find_cstring_character(src, '\0');
  SIZE_T dest_size = find_cstring_character(dest, '\0');
  check_and_report_range_norec(dest, src_size + dest_size + 1, "write");

  InLibcGuard _ig;
  char *res = REAL(strcat)(clear_id(dest), clear_id(src));

  CHECK(res == clear_id(dest));
  return dest;
}

INTERCEPTOR(char *, strncat, char *dest, const char *src, SIZE_T n) {
  ENSURE_PTSAN_INITED();
  GET_STORE_STACK_TRACE;
  SIZE_T dest_size = find_cstring_character(dest, '\0');
  SIZE_T copy_size = find_bounded_character(src, '\0', n);
  // I *think* this is right, because I think the null terminator
  // doesn't end up counting in either of these
  check_and_report_range_norec(dest, dest_size + copy_size + 1, "write");

  InLibcGuard _ig;
  char *res = REAL(strncat)(clear_id(dest), clear_id(src), n);

  CHECK(res == clear_id(dest));
  return dest;
}

#define SET_ENDPTR_ID(nptr, endptr)                                            \
  if (endptr != nullptr) {                                                     \
    auto **real_endptr = clear_id(endptr);                                     \
    *real_endptr = copy_id(*real_endptr, nptr);                                \
  }

#define INTERCEPTOR_STRTO(ret_type, func, char_type)                           \
  INTERCEPTOR(ret_type, func, const char_type *nptr, char_type **endptr) {     \
    ENSURE_PTSAN_INITED();                                                     \
    ret_type res = REAL(func)(clear_id(nptr), clear_id(endptr));               \
    SET_ENDPTR_ID(nptr, endptr);                                               \
    return res;                                                                \
  }

#define INTERCEPTOR_STRTO_BASE(ret_type, func, char_type)                      \
  INTERCEPTOR(ret_type, func, const char_type *nptr, char_type **endptr,       \
              int base) {                                                      \
    ENSURE_PTSAN_INITED();                                                     \
    ret_type res = REAL(func)(clear_id(nptr), clear_id(endptr), base);         \
    SET_ENDPTR_ID(nptr, endptr);                                               \
    return res;                                                                \
  }

#define INTERCEPTOR_STRTO_LOC(ret_type, func, char_type)                       \
  INTERCEPTOR(ret_type, func, const char_type *nptr, char_type **endptr,       \
              void *loc) {                                                     \
    ENSURE_PTSAN_INITED();                                                     \
    ret_type res = REAL(func)(clear_id(nptr), clear_id(endptr), loc);          \
    SET_ENDPTR_ID(nptr, endptr);                                               \
    return res;                                                                \
  }

#define INTERCEPTOR_STRTO_BASE_LOC(ret_type, func, char_type)                  \
  INTERCEPTOR(ret_type, func, const char_type *nptr, char_type **endptr,       \
              int base, void *loc) {                                           \
    ENSURE_PTSAN_INITED();                                                     \
    ret_type res = REAL(func)(clear_id(nptr), clear_id(endptr), base, loc);    \
    SET_ENDPTR_ID(nptr, endptr);                                               \
    return res;                                                                \
  }

#define INTERCEPTORS_STRTO(ret_type, func, char_type)      \
  INTERCEPTOR_STRTO(ret_type, func, char_type)             \
  INTERCEPTOR_STRTO_LOC(ret_type, func##_l, char_type)     \
  INTERCEPTOR_STRTO_LOC(ret_type, __##func##_l, char_type) \
  INTERCEPTOR_STRTO_LOC(ret_type, __##func##_internal, char_type)

#define INTERCEPTORS_STRTO_BASE(ret_type, func, char_type)      \
  INTERCEPTOR_STRTO_BASE(ret_type, func, char_type)             \
  INTERCEPTOR_STRTO_BASE_LOC(ret_type, func##_l, char_type)     \
  INTERCEPTOR_STRTO_BASE_LOC(ret_type, __##func##_l, char_type) \
  INTERCEPTOR_STRTO_BASE_LOC(ret_type, __##func##_internal, char_type)

INTERCEPTORS_STRTO(double, strtod, char)
INTERCEPTORS_STRTO(float, strtof, char)
INTERCEPTORS_STRTO(long double, strtold, char)
INTERCEPTORS_STRTO_BASE(long, strtol, char)
INTERCEPTORS_STRTO_BASE(long long, strtoll, char)
INTERCEPTORS_STRTO_BASE(unsigned long, strtoul, char)
INTERCEPTORS_STRTO_BASE(unsigned long long, strtoull, char)
INTERCEPTORS_STRTO_BASE(u64, strtouq, char)

INTERCEPTORS_STRTO(double, wcstod, wchar_t)
INTERCEPTORS_STRTO(float, wcstof, wchar_t)
INTERCEPTORS_STRTO(long double, wcstold, wchar_t)
INTERCEPTORS_STRTO_BASE(long, wcstol, wchar_t)
INTERCEPTORS_STRTO_BASE(long long, wcstoll, wchar_t)
INTERCEPTORS_STRTO_BASE(unsigned long, wcstoul, wchar_t)
INTERCEPTORS_STRTO_BASE(unsigned long long, wcstoull, wchar_t)

#if SANITIZER_GLIBC
INTERCEPTORS_STRTO(double, __isoc23_strtod, char)
INTERCEPTORS_STRTO(float, __isoc23_strtof, char)
INTERCEPTORS_STRTO(long double, __isoc23_strtold, char)
INTERCEPTORS_STRTO_BASE(long, __isoc23_strtol, char)
INTERCEPTORS_STRTO_BASE(long long, __isoc23_strtoll, char)
INTERCEPTORS_STRTO_BASE(unsigned long, __isoc23_strtoul, char)
INTERCEPTORS_STRTO_BASE(unsigned long long, __isoc23_strtoull, char)
INTERCEPTORS_STRTO_BASE(u64, __isoc23_strtouq, char)

INTERCEPTORS_STRTO(double, __isoc23_wcstod, wchar_t)
INTERCEPTORS_STRTO(float, __isoc23_wcstof, wchar_t)
INTERCEPTORS_STRTO(long double, __isoc23_wcstold, wchar_t)
INTERCEPTORS_STRTO_BASE(long, __isoc23_wcstol, wchar_t)
INTERCEPTORS_STRTO_BASE(long long, __isoc23_wcstoll, wchar_t)
INTERCEPTORS_STRTO_BASE(unsigned long, __isoc23_wcstoul, wchar_t)
INTERCEPTORS_STRTO_BASE(unsigned long long, __isoc23_wcstoull, wchar_t)
#endif

#define INTERCEPTORS_ATO(name, rettype)                       \
  INTERCEPTOR(rettype, name, const char *nptr) {                 \
    ENSURE_PTSAN_INITED();                                       \
    return REAL(name)(clear_id(nptr));                           \
  }

INTERCEPTORS_ATO(atoi,        int)
INTERCEPTORS_ATO(atol,        long)
INTERCEPTORS_ATO(atoll,       long long)
INTERCEPTORS_ATO(atof,        float)


#define INTERCEPT_STRTO(func) \
  INTERCEPT_FUNCTION(func); \
  INTERCEPT_FUNCTION(func##_l); \
  INTERCEPT_FUNCTION(__##func##_l); \
  INTERCEPT_FUNCTION(__##func##_internal);

// wprintfs are in interceptors_format.cpp

#define INTERCEPTOR_STRFTIME_BODY(char_type, ret_type, func, s, ...) \
  ENSURE_PTSAN_INITED();                                             \
  return doPTSanCall(REAL(func), s, __VA_ARGS__);

INTERCEPTOR(SIZE_T, strftime, char *s, SIZE_T max, const char *format,
            __sanitizer_tm *tm) {
  INTERCEPTOR_STRFTIME_BODY(char, SIZE_T, strftime, s, max, format, tm);
}

INTERCEPTOR(SIZE_T, strftime_l, char *s, SIZE_T max, const char *format,
            __sanitizer_tm *tm, void *loc) {
  INTERCEPTOR_STRFTIME_BODY(char, SIZE_T, strftime_l, s, max, format, tm, loc);
}

INTERCEPTOR(SIZE_T, __strftime_l, char *s, SIZE_T max, const char *format,
            __sanitizer_tm *tm, void *loc) {
  INTERCEPTOR_STRFTIME_BODY(char, SIZE_T, __strftime_l, s, max, format, tm,
                            loc);
}

INTERCEPTOR(SIZE_T, wcsftime, wchar_t *s, SIZE_T max, const wchar_t *format,
            __sanitizer_tm *tm) {
  INTERCEPTOR_STRFTIME_BODY(wchar_t, SIZE_T, wcsftime, s, max, format, tm);
}

INTERCEPTOR(SIZE_T, wcsftime_l, wchar_t *s, SIZE_T max, const wchar_t *format,
            __sanitizer_tm *tm, void *loc) {
  INTERCEPTOR_STRFTIME_BODY(wchar_t, SIZE_T, wcsftime_l, s, max, format, tm,
                            loc);
}

INTERCEPTOR(SIZE_T, __wcsftime_l, wchar_t *s, SIZE_T max, const wchar_t *format,
            __sanitizer_tm *tm, void *loc) {
  INTERCEPTOR_STRFTIME_BODY(wchar_t, SIZE_T, __wcsftime_l, s, max, format, tm,
                            loc);
}


INTERCEPTOR(wchar_t *, wmemcpy, wchar_t *dest, const wchar_t *src, SIZE_T n) {
  ENSURE_PTSAN_INITED();
  GET_STORE_STACK_TRACE;

  SIZE_T sz = n * sizeof(wchar_t);
  check_and_report_range(dest, sz, "write");
  check_and_report_range(src, sz, "read");
  n = sz / sizeof(wchar_t);

  InLibcGuard _ig;
  wchar_t *res = REAL(wmemcpy)(clear_id(dest), clear_id(src), n);

  CHECK(res == clear_id(dest));
  return dest;
}

INTERCEPTOR(wchar_t *, wmempcpy, wchar_t *dest, const wchar_t *src, SIZE_T n) {
  ENSURE_PTSAN_INITED();
  GET_STORE_STACK_TRACE;

  SIZE_T sz = n * sizeof(wchar_t);
  check_and_report_range(dest, sz, "write");
  check_and_report_range(src, sz, "read");
  n = sz / sizeof(wchar_t);

  InLibcGuard _ig;
  wchar_t *res = REAL(wmempcpy)(clear_id(dest), clear_id(src), n);

  return copy_id(res, dest);
}

INTERCEPTOR(wchar_t *, wmemset, wchar_t *s, wchar_t c, SIZE_T n) {
  ENSURE_PTSAN_INITED();

  SIZE_T sz = n * sizeof(wchar_t);
  check_and_report_range(s, sz, "write");
  n = sz / sizeof(wchar_t);

  InLibcGuard _ig;
  wchar_t *res = REAL(wmemset)(clear_id(s), c, n);

  CHECK(res = clear_id(s));
  return s;
}

INTERCEPTOR(wchar_t *, wmemmove, wchar_t *dest, const wchar_t *src, SIZE_T n) {
  ENSURE_PTSAN_INITED();
  GET_STORE_STACK_TRACE;

  SIZE_T sz = n * sizeof(wchar_t);
  check_and_report_range(dest, sz, "write");
  check_and_report_range(src, sz, "read");
  n = sz / sizeof(wchar_t);

  InLibcGuard _ig;
  wchar_t *res = REAL(wmemmove)(clear_id(dest), clear_id(src), n);

  CHECK(res == clear_id(dest));
  return dest;
}

INTERCEPTOR(int, wcscmp, const wchar_t *s1, const wchar_t *s2) {
  ENSURE_PTSAN_INITED();

  // XXX: check size of access here?

  InLibcGuard _ig;
  int res = REAL(wcscmp)(clear_id(s1), clear_id(s2));

  return res;
}

INTERCEPTOR(int, gettimeofday, void *tv, void *tz) {
  ENSURE_PTSAN_INITED();
  return doPTSanCall(REAL(gettimeofday), tv, tz);
}

INTERCEPTOR(char *, fcvt, double x, int a, int *b, int *c) {
  ENSURE_PTSAN_INITED();
  InLibcGuard _ig;
  char *res = REAL(fcvt)(x, a, clear_id(b), clear_id(c));

  // Res is a shared buffer(?) so we don't mess with ID for now
  // XXX: Figure out how to put an ID on this -- probably
  // a permanently-allocated one?
  return res;
}

INTERCEPTOR(int, fstat, int fd, void *buf) {
  return doPTSanCall(REAL(fstat), fd, buf);
}

INTERCEPTOR(int, fstat64, int fd, void *buf) {
  return doPTSanCall(REAL(fstat64), fd, buf);
}

INTERCEPTOR(int, __fxstat, int magic, int fd, void *buf) {
  return doPTSanCall(REAL(__fxstat), magic, fd, buf);
}

INTERCEPTOR(int, __fxstat64, int magic, int fd, void *buf) {
  return doPTSanCall(REAL(__fxstat64), magic, fd, buf);
}

INTERCEPTOR(int, fstatat, int fd, char *pathname, void *buf, int flags) {
  return doPTSanCall(REAL(fstatat), fd, pathname, buf, flags);
}

INTERCEPTOR(int, fstatat64, int fd, char *pathname, void *buf, int flags) {
  return doPTSanCall(REAL(fstatat64), fd, pathname, buf, flags);
}

INTERCEPTOR(int, __fxstatat, int magic, int fd, char *pathname, void *buf,
            int flags) {
  return doPTSanCall(REAL(__fxstatat), magic, fd, pathname, buf, flags);
}

INTERCEPTOR(int, __fxstatat64, int magic, int fd, char *pathname, void *buf,
            int flags) {
  return doPTSanCall(REAL(__fxstatat64), magic, fd, pathname, buf, flags);
}

// XXX: msan after stats (sans fstat)
// XXX: Getline needs a custom handler because it allocates (?)

/////////////////////////////////////
// SUPER SIMPLE INTERCEPTORS
// these just need a strip and/or ptsan_in_libc
////////////////////////////////////

INTERCEPTOR(void, __assert_fail,
            const char *expr, const char *file,
            unsigned int line, const char *func) {
  ENSURE_PTSAN_INITED();

  InLibcGuard _ig;
  REAL(__assert_fail)(clear_id(expr), clear_id(file), line, clear_id(func));
}

INTERCEPTOR(void, perror, const char *s) {
  ENSURE_PTSAN_INITED();

  InLibcGuard _ig;
  REAL(perror)( clear_id(s) );
}

INTERCEPTOR(void, __GI_perror, const char *s) {
  ENSURE_PTSAN_INITED();

  InLibcGuard _ig;
  REAL(__GI_perror)( clear_id(s) );
}

// XXX: do we need to recurse into addr for weird cases?
INTERCEPTOR(int, bind, int sockfd, const struct sockaddr *addr, SIZE_T addrlen) {
  ENSURE_PTSAN_INITED();

  InLibcGuard _ig;
  int res = REAL(bind)(sockfd, clear_id(addr), addrlen);

  return res;
}

INTERCEPTOR(int, wcswidth, const wchar_t *s, SIZE_T n) {
  ENSURE_PTSAN_INITED();

  InLibcGuard _ig;
  int res = REAL(wcswidth)(clear_id(s), n);

  return res;
}

INTERCEPTOR(int, wcsnwidth, const wchar_t *s, SIZE_T n) {
  ENSURE_PTSAN_INITED();
  InLibcGuard _ig;
  int res = REAL(wcsnwidth)(clear_id(s), n);
  return res;
}

#define INTERCEPT_SIMPLE                                                       \
  INTERCEPT_FUNCTION(__assert_fail);                                           \
  INTERCEPT_FUNCTION(perror);                                                  \
  INTERCEPT_FUNCTION(__GI_perror);                                             \
  INTERCEPT_FUNCTION(bind);                                                    \
  INTERCEPT_FUNCTION(wcswidth);                                                \
  INTERCEPT_FUNCTION(wcsnwidth);                                               \

// XXX: there's probably a nicer way to do this?
namespace __interception {
  void *__ptsan_real_memcpy(void *dest, const void *src, SIZE_T n) {
    return __ptsan_memcpy(dest, src, n);
  }

  void *__ptsan_real_memset(void *s, int c, SIZE_T n) {
    return __ptsan_memset(s, c, n);
  }

  void *__ptsan_real_memmove(void *dest, const void *src, SIZE_T n) {
    return __ptsan_memmove(dest, src, n);
  }
}


/////////////////////////////////////
// THREADING STUFF
////////////////////////////////////
extern "C" int pthread_attr_init(void *attr);
extern "C" int pthread_attr_destroy(void *attr);

static void *PTSanThreadStartFunc(void *arg) {
  // This runs at the start of the new thread, so ptsan_in_libc isn't inherited
  // from pthread_create
  PTSanThread *t;
  {
    // guard must end before ThreadStart(): that's user code and must see ptsan_in_libc == 0
    InLibcGuard _ig;
    t = clear_id((PTSanThread *)arg);
    t->Init();
    SetCurrentThread(t);
    t->InitTLS();
    SetSigProcMask(&t->starting_sigset_, nullptr);
    if (ClLAM)
      ptsan_enable_lam_u48();
  }

  void * out = t->ThreadStart();
  return out;
}

INTERCEPTOR(int, pthread_create, void *th, void *attr, void *(*callback)(void*),
            void * param) {
  ENSURE_PTSAN_INITED(); // for GetTlsSize()
  InLibcGuard _ig;

  __sanitizer_pthread_attr_t myattr;
  if (!attr) {
    pthread_attr_init(&myattr);
    attr = &myattr;
  }

  AdjustStackSize(attr);

  PTSanThread *t = PTSanThread::Create(callback, param);
  ScopedBlockSignals block(&t->starting_sigset_);

  int res = REAL(pthread_create)(clear_id(th), clear_id(attr), PTSanThreadStartFunc, t);
  if (res != 0)
    t->Release();

  if (attr == &myattr) {
    pthread_attr_destroy(&myattr);
  }

  return res;
}

INTERCEPTOR(int, epoll_wait,
            int epfd,
            struct epoll_event *events,
            int maxevents,
            int timeout) {
  ENSURE_PTSAN_INITED();
  InLibcGuard _ig;

  struct epoll_event *clear_events = clear_id(events);

  int res = REAL(epoll_wait)(epfd, clear_events, maxevents, timeout);

  return res;
}


#if 0
#define COMMON_INTERCEPTOR_GET_TLS_RANGE(begin, end)                           \
  if (PTSanThread *t = GetCurrentThread()) {                                    \
    *begin = t->tls_begin();                                                   \
    *end = t->tls_end();                                                       \
  } else {                                                                     \
    *begin = *end = 0;                                                         \
  }
#endif

/////////////////////////////////////////////////
// DEFINITIONS/MACROS FOR COMMON INTERCEPTORS
/////////////////////////////////////////////////

int OnExit() {
  // FIXME: ask frontend whether we need to return failure.
  return 0;
}

# define ORIG_REAL(x) __interception::PTR_TO_REAL(x)

// Stolen from MSan. Seems like a good idea to avoid absurd WTF questions later
#define PTSAN_INTERCEPT_FUNC(name)                                             \
  do {                                                                         \
    if (!INTERCEPT_FUNCTION(name))                                             \
      VReport(1, "PointerTagSanitizer: failed to intercept '%s'\n", #name);    \
  } while (0)

#define PTSAN_INTERCEPT_FUNC_VER(name, ver)                                    \
  do {                                                                         \
    if (!INTERCEPT_FUNCTION_VER(name, ver))                                    \
      VReport(1, "PointerTagSanitizer: failed to intercept '%s@@%s'\n", #name, \
              ver);                                                            \
  } while (0)
#define PTSAN_INTERCEPT_FUNC_VER_UNVERSIONED_FALLBACK(name, ver)               \
  do {                                                                         \
    if (!INTERCEPT_FUNCTION_VER(name, ver) && !INTERCEPT_FUNCTION(name))       \
      VReport(1,                                                               \
              "PointerTagSanitizer: failed to intercept '%s@@%s' or '%s'\n",   \
              #name, ver, #name);                                              \
  } while (0)

// These macros are all used in sanitizer_common_interceptors.inc, which
// we then include after defining them. See the top of that file for usage
#define COMMON_INTERCEPT_FUNCTION(name) PTSAN_INTERCEPT_FUNC(name)
#define COMMON_INTERCEPT_FUNCTION_VER(name, ver) \
  PTSAN_INTERCEPT_FUNC_VER(name, ver)
#define COMMON_INTERCEPT_FUNCTION_VER_UNVERSIONED_FALLBACK(name, ver) \
  PTSAN_INTERCEPT_FUNC_VER_UNVERSIONED_FALLBACK(name, ver)
#define COMMON_INTERCEPTOR_UNPOISON_PARAM(count) {}
#define COMMON_INTERCEPTOR_WRITE_RANGE(ctx, ptr, size) \
  check_and_report_range_norec(ptr, size, "write")
#define COMMON_INTERCEPTOR_READ_RANGE(ctx, ptr, size) \
  check_and_report_range_norec(ptr, size, "read")
#if 0 // I worry this might leak IDs, which we really don't want
#define COMMON_INTERCEPTOR_INITIALIZE_RANGE(ptr, size) \
  if(get_id(ptr) == 0) { ptr = tag_with_next_id(ptr, size); }
#else
#define COMMON_INTERCEPTOR_INITIALIZE_RANGE(ptr, size) {}
#endif
#define COMMON_INTERCEPTOR_ENTER(ctx, func, ...)                               \
  if (ptsan_init_is_running)                                                   \
    return ORIG_REAL(func)(__VA_ARGS__);                                       \
  ENSURE_PTSAN_INITED();                                                       \
  ctx = nullptr; \
  (void)(ctx);
#define COMMON_INTERCEPTOR_DIR_ACQUIRE(ctx, path) \
  do {                                            \
  } while (false)
#define COMMON_INTERCEPTOR_FD_ACQUIRE(ctx, fd) \
  do {                                         \
  } while (false)
#define COMMON_INTERCEPTOR_FD_RELEASE(ctx, fd) \
  do {                                         \
  } while (false)
#define COMMON_INTERCEPTOR_FD_SOCKET_ACCEPT(ctx, fd, newfd) \
  do {                                                      \
  } while (false)
#define COMMON_INTERCEPTOR_SET_THREAD_NAME(ctx, name) \
  do {                                                \
  } while (false)
#define COMMON_INTERCEPTOR_SET_PTHREAD_NAME(ctx, thread, name) \
  do {                                                         \
  } while (false)
#define COMMON_INTERCEPTOR_BLOCK_REAL(name) REAL(name)
#define COMMON_INTERCEPTOR_ON_EXIT(ctx) OnExit()
#define COMMON_INTERCEPTOR_LIBRARY_LOADED(filename, handle)                    \
  do {                                                                         \
  } while (false)

#define COMMON_INTERCEPTOR_NOTHING_IS_INITIALIZED (!ptsan_initialized)

#define COMMON_INTERCEPTOR_MEMSET_IMPL(ctx, block, c, size) \
  {                                                         \
    (void)ctx;                                              \
    return __ptsan_memset(block, c, size);                   \
  }
#define COMMON_INTERCEPTOR_MEMMOVE_IMPL(ctx, to, from, size) \
  {                                                          \
    (void)ctx;                                               \
    return __ptsan_memmove(to, from, size);                   \
  }
#define COMMON_INTERCEPTOR_MEMCPY_IMPL(ctx, to, from, size) \
  {                                                         \
    (void)ctx;                                              \
    return __ptsan_memcpy(to, from, size);                   \
  }

#define COMMON_INTERCEPTOR_COPY_STRING(ctx, to, from, size) \
  do {                                                      \
  } while (false)

#define COMMON_INTERCEPTOR_DLOPEN(filename, flag)                              \
  ({                                                                           \
    CheckNoDeepBind(filename, flag);                                           \
    ptsan_in_libc = 1;                                                         \
    void * out = ORIG_REAL(dlopen)(stripID(filename), flag);                   \
    ptsan_in_libc = 0;                                                         \
    out;                                                                      \
  })

// Include these here, before we get to the really crazy redefinitions
#include "sanitizer_common/sanitizer_common_interceptors_memintrinsics.inc"

// Now we get to our crazy default wrapping:

#define STRIP_PARENS(...) __VA_ARGS__

// Set up our wrapper. Just adds a call to doPTSanCall. The extra level of indirection
// is necessary because a.) writing long macros sucks and b.) apparently `if constexpr`
// doesn't want to be in a c-style macro(?)
#define DEFINE_PTSAN_REAL(ret_type, func, args, ...)                                      \
  namespace __interception {                                                   \
  ret_type __ptsan_real_##func(__VA_ARGS__) {   \
    return doPTSanCall(real_##func STRIP_PARENS args);                                 \
  }                                                                            \
  }

// Based on INTERCEPTOR, with an extra definition in the middle
#define PTSAN_INTERCEPTOR(ret_type, func, args, ...)        \
  DEFINE_REAL(ret_type, func, __VA_ARGS__)      \
  DEFINE_PTSAN_REAL(ret_type, func, args, __VA_ARGS__)             \
  DECLARE_WRAPPER(ret_type, func, __VA_ARGS__)  \
  extern "C" INTERCEPTOR_ATTRIBUTE ret_type WRAP(func)(__VA_ARGS__)

#define PTSAN_INTERCEPTOR_WITH_SUFFIX(ret_type, func, args, ...) \
  PTSAN_INTERCEPTOR(ret_type, func, args, __VA_ARGS__)

// Redefne REAL so it points to our wrapper instead of the default
#undef REAL
#define PTR_TO_PTSAN_REAL(x) __ptsan_real_##x
#define REAL(x) __interception::PTR_TO_PTSAN_REAL(x)

// These scanners validate logical reads before accessing memory. Keep their
// PTSan-specific implementations here and disable the generated versions below.
static inline int CharCmpX(unsigned char c1, unsigned char c2) {
  return (c1 == c2) ? 0 : (c1 < c2) ? -1 : 1;
}

PTSAN_INTERCEPTOR(SIZE_T, strlen, (, s), const char *s) {
  // Sometimes strlen is called prior to InitializeCommonInterceptors,
  // in which case the REAL(strlen) typically used in
  // COMMON_INTERCEPTOR_ENTER will fail.  We use internal_strlen here
  // to handle that.
  if (COMMON_INTERCEPTOR_NOTHING_IS_INITIALIZED) {
    return internal_strlen(s);
  }
  void *ctx;
  COMMON_INTERCEPTOR_ENTER(ctx, strlen, s);
  if (common_flags()->intercept_strlen) {
    return find_cstring_character(s, '\0');
  }
  return REAL(strlen)(s);
}

PTSAN_INTERCEPTOR(SIZE_T, strnlen, (, s, maxlen), const char *s, SIZE_T maxlen) {
  void *ctx;
  COMMON_INTERCEPTOR_ENTER(ctx, strnlen, s, maxlen);
  if (common_flags()->intercept_strlen) {
    return find_bounded_character(s, '\0', maxlen);
  }
  return REAL(strnlen)(s, maxlen);
}

#if SANITIZER_INTERCEPTOR_HOOKS
SANITIZER_INTERFACE_WEAK_DEF(void, __sanitizer_weak_hook_strcmp, uptr called_pc,
                            const char *s1, const char *s2, int result) {}
#endif
PTSAN_INTERCEPTOR(int, strcmp, (, s1, s2), const char *s1, const char *s2) {
  void *ctx;
  COMMON_INTERCEPTOR_ENTER(ctx, strcmp, s1, s2);
  const uptr left_bytes = bytes_remaining(s1);
  const uptr right_bytes = bytes_remaining(s2);
  unsigned char c1, c2;
  uptr i;
  for (i = 0;; i++) {
    if (common_flags()->intercept_strcmp) {
      if (i == left_bytes) {
        report_scan_read(s1, i);
      }
      if (i == right_bytes) {
        report_scan_read(s2, i);
      }
    }
    c1 = (unsigned char)s1[i];
    c2 = (unsigned char)s2[i];
    if (c1 != c2 || c1 == '\0') {
      break;
    }
  }
  if (common_flags()->intercept_strcmp) {
    if (common_flags()->strict_string_checks) {
      find_cstring_character(s1, '\0');
      find_cstring_character(s2, '\0');
    }
  }
  int result = CharCmpX(c1, c2);
#if SANITIZER_INTERCEPTOR_HOOKS
  __sanitizer_weak_hook_strcmp(GET_CALLER_PC(), s1, s2, result);
#endif
  return result;
}

#if SANITIZER_INTERCEPTOR_HOOKS
SANITIZER_INTERFACE_WEAK_DEF(void, __sanitizer_weak_hook_strncmp, uptr called_pc,
                            const char *s1, const char *s2, uptr n,
                            int result) {}
#endif
PTSAN_INTERCEPTOR(int, strncmp, (, s1, s2, size), const char *s1,
                  const char *s2, uptr size) {
  if (COMMON_INTERCEPTOR_NOTHING_IS_INITIALIZED) {
    return internal_strncmp(s1, s2, size);
  }
  void *ctx;
  COMMON_INTERCEPTOR_ENTER(ctx, strncmp, s1, s2, size);
  const uptr left_bytes = bytes_remaining(s1);
  const uptr right_bytes = bytes_remaining(s2);
  unsigned char c1 = 0, c2 = 0;
  uptr i;
  for (i = 0; i < size; i++) {
    if (i == left_bytes) {
      report_scan_read(s1, i);
    }
    if (i == right_bytes) {
      report_scan_read(s2, i);
    }
    c1 = (unsigned char)s1[i];
    c2 = (unsigned char)s2[i];
    if (c1 != c2 || c1 == '\0') {
      break;
    }
  }
  if (common_flags()->strict_string_checks) {
    find_bounded_character(s1, '\0', size);
    find_bounded_character(s2, '\0', size);
  }
  int result = CharCmpX(c1, c2);
#if SANITIZER_INTERCEPTOR_HOOKS
  __sanitizer_weak_hook_strncmp(GET_CALLER_PC(), s1, s2, size, result);
#endif
  return result;
}


static char *checkedStrchr(const char *s, int c) {
  const uptr offset = find_cstring_character(s, c);
  if (common_flags()->strict_string_checks) {
    find_cstring_character(s, '\0');
  }
  const char *result = clear_id(s) + offset;
  return (unsigned char)*result == (unsigned char)c
             ? copy_id((char *)result, s) : nullptr;
}

PTSAN_INTERCEPTOR(char*, strchr, (, s, c), const char *s, int c) {
  void *ctx;
  if (COMMON_INTERCEPTOR_NOTHING_IS_INITIALIZED) {
    return internal_strchr(s, c);
  }
  COMMON_INTERCEPTOR_ENTER(ctx, strchr, s, c);
  if (common_flags()->intercept_strchr) {
    return checkedStrchr(s, c);
  }
  return REAL(strchr)(s, c);
}

PTSAN_INTERCEPTOR(char*, index, (, s, c), const char *s, int c) {
  void *ctx;
  if (COMMON_INTERCEPTOR_NOTHING_IS_INITIALIZED) {
    return internal_strchr(s, c);
  }
  COMMON_INTERCEPTOR_ENTER(ctx, index, s, c);
  if (common_flags()->intercept_strchr) {
    return checkedStrchr(s, c);
  }
  return REAL(index)(s, c);
}


PTSAN_INTERCEPTOR(char*, strchrnul, (, s, c), const char *s, int c) {
  void *ctx;
  COMMON_INTERCEPTOR_ENTER(ctx, strchrnul, s, c);
  if (common_flags()->intercept_strchr) {
    const uptr offset = find_cstring_character(s, c);
    if (common_flags()->strict_string_checks) {
      find_cstring_character(s, '\0');
    }
    return copy_id((char *)clear_id(s) + offset, s);
  }
  return REAL(strchrnul)(s, c);
}

#if SANITIZER_INTERCEPTOR_HOOKS
SANITIZER_INTERFACE_WEAK_DEF(void, __sanitizer_weak_hook_memcmp, uptr called_pc,
                            const void *s1, const void *s2, uptr n,
                            int result) {}
#endif
static int checkedMemcmp(const void *a1, const void *a2, uptr size,
                         int (*real)(const void *, const void *, uptr)) {
  if (common_flags()->intercept_memcmp) {
    if (common_flags()->strict_memcmp) {
      // Check both entire regions even if their first bytes differ.
      check_and_report_range_norec(a1, size, "read");
      check_and_report_range_norec(a2, size, "read");
    } else {
      const unsigned char *s1 = (const unsigned char *)a1;
      const unsigned char *s2 = (const unsigned char *)a2;
      const uptr left_bytes = bytes_remaining(a1);
      const uptr right_bytes = bytes_remaining(a2);
      for (uptr i = 0; i < size; ++i) {
        if (i == left_bytes) {
          report_scan_read(a1, i);
        }
        if (i == right_bytes) {
          report_scan_read(a2, i);
        }
        unsigned char c1 = s1[i];
        unsigned char c2 = s2[i];
        if (c1 != c2) {
          return CharCmpX(c1, c2);
        }
      }
      return 0;
    }
  }
  return real(a1, a2, size);
}

PTSAN_INTERCEPTOR(int, memcmp, (, a1, a2, size), const void *a1,
                  const void *a2, uptr size) {
  if (COMMON_INTERCEPTOR_NOTHING_IS_INITIALIZED) {
    return internal_memcmp(a1, a2, size);
  }
  void *ctx;
  COMMON_INTERCEPTOR_ENTER(ctx, memcmp, a1, a2, size);
  int result = checkedMemcmp(a1, a2, size, REAL(memcmp));
#if SANITIZER_INTERCEPTOR_HOOKS
  __sanitizer_weak_hook_memcmp(GET_CALLER_PC(), a1, a2, size, result);
#endif
  return result;
}

PTSAN_INTERCEPTOR(int, bcmp, (, a1, a2, size), const void *a1,
                  const void *a2, uptr size) {
  if (COMMON_INTERCEPTOR_NOTHING_IS_INITIALIZED) {
    return internal_memcmp(a1, a2, size);
  }
  void *ctx;
  COMMON_INTERCEPTOR_ENTER(ctx, bcmp, a1, a2, size);
  int result = checkedMemcmp(a1, a2, size, REAL(bcmp));
#if SANITIZER_INTERCEPTOR_HOOKS
  __sanitizer_weak_hook_memcmp(GET_CALLER_PC(), a1, a2, size, result);
#endif
  return result;
}

PTSAN_INTERCEPTOR(void*, memchr, (, s, c, n), const void *s, int c, SIZE_T n) {
  if (COMMON_INTERCEPTOR_NOTHING_IS_INITIALIZED) {
    return internal_memchr(s, c, n);
  }
  void *ctx;
  COMMON_INTERCEPTOR_ENTER(ctx, memchr, s, c, n);
  const uptr offset = find_bounded_character(s, c, n);
  return offset == n ? nullptr
                     : copy_id((char *)clear_id(s) + offset, s);
}

// Okay, all the setup is done. Any special-casing on mostly-common libc wrappers
// goes here. The definitions should be removed from ptsan_common_interceptors.inc:

INTERCEPTOR(__sanitizer_sem_t *, sem_open,
                  const char *name, int oflag, ...) {
  void *ctx;
  va_list ap;
  va_start(ap, oflag);
  u32 mode = va_arg(ap, u32);
  u32 value = va_arg(ap, u32);
  COMMON_INTERCEPTOR_ENTER(ctx, sem_open, name, oflag, mode, value);
  COMMON_INTERCEPTOR_READ_RANGE(ctx, name, internal_strlen(name) + 1);
  __sanitizer_sem_t *s = ORIG_REAL(sem_open)(stripID(name), oflag, mode, value);
  if (s)
    COMMON_INTERCEPTOR_INITIALIZE_RANGE(s, sizeof(*s));
  va_end(ap);
  return s;
}

#if SANITIZER_INTERCEPT_GETLINE
// Should probably macro these three, but eeeeh
INTERCEPTOR(SSIZE_T, getline, char **lineptr, SIZE_T *n, void *stream) {
  void *ctx;
  COMMON_INTERCEPTOR_ENTER(ctx, getline, lineptr, n, stream);
  // FIXME: Should ideally check-then-call
  // https://github.com/google/sanitizers/issues/321.
  char *real_line = nullptr;
  char *old_line = nullptr;
  char **real_lineptr = stripID(lineptr);
  if (*real_lineptr != nullptr) {
    real_line = stripID(*real_lineptr);
    old_line = *real_lineptr;
  }

  // guard only the libc call: tag_realloc below is a NOP while ptsan_in_libc > 0
  SSIZE_T res;
  {
    InLibcGuard _ig;
    res = ORIG_REAL(getline)(&real_line, stripID(n), stripID(stream));
  }

  if (res <= 0) {
    return res;
  }

  *real_lineptr = tag_realloc(real_line, old_line, *n);

  COMMON_INTERCEPTOR_WRITE_RANGE(ctx, lineptr, sizeof(real_line));
  COMMON_INTERCEPTOR_WRITE_RANGE(ctx, n, sizeof(*n));
  COMMON_INTERCEPTOR_WRITE_RANGE(ctx, *real_lineptr, res + 1);

  return res;
}

INTERCEPTOR(SSIZE_T, __getdelim, char **lineptr, SIZE_T *n, int delim, void *stream) {
  void *ctx;
  COMMON_INTERCEPTOR_ENTER(ctx, getline, lineptr, n, stream);
  // FIXME: Should ideally check-then-call
  // https://github.com/google/sanitizers/issues/321.
  char *real_line = nullptr;
  char *old_line = nullptr;
  char **real_lineptr = stripID(lineptr);
  if (*real_lineptr != nullptr) {
    real_line = stripID(*real_lineptr);
    old_line = *real_lineptr;
  }

  SSIZE_T res;
  {
    InLibcGuard _ig;
    res = ORIG_REAL(__getdelim)(&real_line, stripID(n), delim, stripID(stream));
  }

  if (res <= 0) {
    return res;
  }

  *real_lineptr = tag_realloc(real_line, old_line, *n);

  COMMON_INTERCEPTOR_WRITE_RANGE(ctx, lineptr, sizeof(real_line));
  COMMON_INTERCEPTOR_WRITE_RANGE(ctx, n, sizeof(*n));
  COMMON_INTERCEPTOR_WRITE_RANGE(ctx, *real_lineptr, res + 1);

  return res;
}

INTERCEPTOR(SSIZE_T, getdelim, char **lineptr, SIZE_T *n, int delim, void *stream) {
  void *ctx;
  COMMON_INTERCEPTOR_ENTER(ctx, getline, lineptr, n, stream);
  // FIXME: Should ideally check-then-call
  // https://github.com/google/sanitizers/issues/321.
  char *real_line = nullptr;
  char *old_line = nullptr;
  char **real_lineptr = stripID(lineptr);
  if (*real_lineptr != nullptr) {
    real_line = stripID(*real_lineptr);
    old_line = *real_lineptr;
  }

  SSIZE_T res;
  {
    InLibcGuard _ig;
    res = ORIG_REAL(__getdelim)(&real_line, stripID(n), delim, stripID(stream));
  }

  if (res <= 0) {
    return res;
  }

  *real_lineptr = tag_realloc(real_line, old_line, *n);

  COMMON_INTERCEPTOR_WRITE_RANGE(ctx, lineptr, sizeof(real_line));
  COMMON_INTERCEPTOR_WRITE_RANGE(ctx, n, sizeof(*n));
  COMMON_INTERCEPTOR_WRITE_RANGE(ctx, *real_lineptr, res + 1);

  return res;
}
#endif

// We handle strings/formatting ourselves above
#undef SANITIZER_INTERCEPT_SCANF
#define SANITIZER_INTERCEPT_SCANF 0
#undef SANITIZER_INTERCEPT_PRINTF
#define SANITIZER_INTERCEPT_PRINTF 0
#undef SANITIZER_INTERCEPT_ISOC99_PRINTF
#define SANITIZER_INTERCEPT_ISOC99_PRINTF 0
#undef SANITIZER_INTERCEPT_ISOC99_SCANF
#define SANITIZER_INTERCEPT_ISOC99_SCANF 0

// We handle these ourselves in ptsan_intercetpors_with_libc_headers
#undef SANITIZER_INTERCEPT_LOCALTIME_AND_FRIENDS
#define SANITIZER_INTERCEPT_LOCALTIME_AND_FRIENDS 0
#undef SANITIZER_INTERCEPT_OBSTACK
#define SANITIZER_INTERCEPT_OBSTACK 0
#undef SANITIZER_INTERCEPT_SETLOCALE
#define SANITIZER_INTERCEPT_SETLOCALE 0
#undef SANITIZER_INTERCEPT_MBSTOWCS
#define SANITIZER_INTERCEPT_MBSTOWCS 0
#undef SANITIZER_INTERCEPT_MBSNRTOWCS
#define SANITIZER_INTERCEPT_MBSNRTOWCS 0
#undef SANITIZER_INTERCEPT_WCSTOMBS
#define SANITIZER_INTERCEPT_WCSTOMBS 0
#undef SANITIZER_INTERCEPT_WCSNRTOMBS
#define SANITIZER_INTERCEPT_WCSNRTOMBS 0
#undef SANITIZER_INTERCEPT_WCRTOMB
#define SANITIZER_INTERCEPT_WCRTOMB 0
#undef SANITIZER_INTERCEPT_WCTOMB
#define SANITIZER_INTERCEPT_WCTOMB 0
#undef SANITIZER_INTERCEPT_PROCESS_VM_READV
#define SANITIZER_INTERCEPT_PROCESS_VM_READV 0
#undef SANITIZER_INTERCEPT_PREADV2
#define SANITIZER_INTERCEPT_PREADV2 0
#undef SANITIZER_INTERCEPT_PWRITEV2
#define SANITIZER_INTERCEPT_PWRITEV2 0
#undef SANITIZER_INTERCEPT_READV
#define SANITIZER_INTERCEPT_READV 0
#undef SANITIZER_INTERCEPT_WRITEV
#define SANITIZER_INTERCEPT_WRITEV 0
#undef SANITIZER_INTERCEPT_PREADV
#define SANITIZER_INTERCEPT_PREADV 0
#undef SANITIZER_INTERCEPT_PWRITEV
#define SANITIZER_INTERCEPT_PWRITEV 0
#undef SANITIZER_INTERCEPT_PREADV64
#define SANITIZER_INTERCEPT_PREADV64 0
#undef SANITIZER_INTERCEPT_PWRITEV64
#define SANITIZER_INTERCEPT_PWRITEV64 0

#undef SANITIZER_INTERCEPT_QSORT_R
#define SANITIZER_INTERCEPT_QSORT_R 0
#undef SANITIZER_INTERCEPT_QSORT
#define SANITIZER_INTERCEPT_QSORT 0

#undef SANITIZER_INTERCEPT_RECVMSG
#define SANITIZER_INTERCEPT_RECVMSG 0
#undef SANITIZER_INTERCEPT_RECVMMSG
#define SANITIZER_INTERCEPT_RECVMMSG 0

#undef SANITIZER_INTERCEPT_MMAP
#define SANITIZER_INTERCEPT_MMAP 0
#undef SANITIZER_INTERCEPT_MMAP64
#define SANITIZER_INTERCEPT_MMAP64 0

// PTSan-specific bounds-aware scanners are defined above.
#undef SANITIZER_INTERCEPT_STRLEN
#define SANITIZER_INTERCEPT_STRLEN 0
#undef SANITIZER_INTERCEPT_STRNLEN
#define SANITIZER_INTERCEPT_STRNLEN 0
#undef SANITIZER_INTERCEPT_STRCMP
#define SANITIZER_INTERCEPT_STRCMP 0
#undef SANITIZER_INTERCEPT_STRCHR
#define SANITIZER_INTERCEPT_STRCHR 0
#undef SANITIZER_INTERCEPT_STRCHRNUL
#define SANITIZER_INTERCEPT_STRCHRNUL 0
#undef SANITIZER_INTERCEPT_MEMCMP
#define SANITIZER_INTERCEPT_MEMCMP 0
#undef SANITIZER_INTERCEPT_BCMP
#define SANITIZER_INTERCEPT_BCMP 0
#undef SANITIZER_INTERCEPT_MEMCHR
#define SANITIZER_INTERCEPT_MEMCHR 0

// And actual libc wrappers include goes here!
#include "ptsan_common_interceptors.inc"

// Now we put REAL back or INTERCEPT_FUNCTION will explode
#undef REAL
#define REAL(X) __interception::PTR_TO_REAL(X)

void __ptsan::InitializeInterceptors() {
  INTERCEPT_FUNCTION(free);
  INTERCEPT_FUNCTION(malloc);
  INTERCEPT_FUNCTION(posix_memalign);
  INTERCEPT_FUNCTION(calloc);
  INTERCEPT_FUNCTION(realloc);
  INTERCEPT_FUNCTION(pthread_create);
  INTERCEPT_FUNCTION(sem_open);
  INTERCEPT_FUNCTION(strlen);
  INTERCEPT_FUNCTION(strnlen);
  INTERCEPT_FUNCTION(strcmp);
  INTERCEPT_FUNCTION(strncmp);
  INTERCEPT_FUNCTION(strchr);
  INTERCEPT_FUNCTION(index);
  INTERCEPT_FUNCTION(strchrnul);
  INTERCEPT_FUNCTION(memcmp);
  INTERCEPT_FUNCTION(bcmp);
  INTERCEPT_FUNCTION(memchr);
  INTERCEPT_FUNCTION(strcpy);
  INTERCEPT_FUNCTION(strncpy);
  INTERCEPT_FUNCTION(stpcpy);
  INTERCEPT_FUNCTION(stpncpy);
  INTERCEPT_FUNCTION(strdup);
  INTERCEPT_FUNCTION(__strdup);
  INTERCEPT_FUNCTION(gcvt);
  INTERCEPT_FUNCTION(strcat);
  INTERCEPT_FUNCTION(strncat);
  INTERCEPT_STRTO(strtod);
  INTERCEPT_STRTO(strtof);
  INTERCEPT_STRTO(strtold);
  INTERCEPT_STRTO(strtol);
  INTERCEPT_STRTO(strtoul);
  INTERCEPT_STRTO(strtoll);
  INTERCEPT_STRTO(strtoull);
  INTERCEPT_STRTO(strtouq);
  INTERCEPT_STRTO(wcstod);
  INTERCEPT_STRTO(wcstof);
  INTERCEPT_STRTO(wcstold);
  INTERCEPT_STRTO(wcstol);
  INTERCEPT_STRTO(wcstoul);
  INTERCEPT_STRTO(wcstoll);
  INTERCEPT_STRTO(wcstoull);
#if SANITIZER_GLIBC
  INTERCEPT_STRTO(__isoc23_strtod);
  INTERCEPT_STRTO(__isoc23_strtof);
  INTERCEPT_STRTO(__isoc23_strtold);
  INTERCEPT_STRTO(__isoc23_strtol);
  INTERCEPT_STRTO(__isoc23_strtoul);
  INTERCEPT_STRTO(__isoc23_strtoll);
  INTERCEPT_STRTO(__isoc23_strtoull);
  INTERCEPT_STRTO(__isoc23_strtouq);
  INTERCEPT_STRTO(__isoc23_wcstod);
  INTERCEPT_STRTO(__isoc23_wcstof);
  INTERCEPT_STRTO(__isoc23_wcstold);
  INTERCEPT_STRTO(__isoc23_wcstol);
  INTERCEPT_STRTO(__isoc23_wcstoul);
  INTERCEPT_STRTO(__isoc23_wcstoll);
  INTERCEPT_STRTO(__isoc23_wcstoull);
#endif
  INTERCEPT_FUNCTION(atoi);
  INTERCEPT_FUNCTION(atol);
  INTERCEPT_FUNCTION(atoll);
  INTERCEPT_FUNCTION(atof);
  INTERCEPT_FUNCTION(strftime);
  INTERCEPT_FUNCTION(strftime_l);
  INTERCEPT_FUNCTION(__strftime_l);
  INTERCEPT_FUNCTION(wcsftime);
  INTERCEPT_FUNCTION(wcsftime_l);
  INTERCEPT_FUNCTION(__wcsftime_l);
  INTERCEPT_FUNCTION(wmemchr);
  INTERCEPT_FUNCTION(wcschr);
  INTERCEPT_FUNCTION(wcsrchr);
  INTERCEPT_FUNCTION(wcsstr);
  INTERCEPT_FUNCTION(wcspbrk);
  INTERCEPT_FUNCTION(wmemcpy);
  INTERCEPT_FUNCTION(wmempcpy);
  INTERCEPT_FUNCTION(wmemset);
  INTERCEPT_FUNCTION(wmemmove);
  INTERCEPT_FUNCTION(wcscmp);
  INTERCEPT_FUNCTION(gettimeofday);
  INTERCEPT_FUNCTION(fcvt);
  INTERCEPT_FUNCTION(fstat);
  INTERCEPT_FUNCTION(fstat64);
  INTERCEPT_FUNCTION(__fxstat);
  INTERCEPT_FUNCTION(__fxstat64);
  INTERCEPT_FUNCTION(fstatat);
  INTERCEPT_FUNCTION(fstatat64);
  INTERCEPT_FUNCTION(__fxstatat);
  INTERCEPT_FUNCTION(__fxstatat64);
  INTERCEPT_FUNCTION(epoll_wait);

  INTERCEPT_SIMPLE;

  InitializeCommonInterceptors();
  InitializeLibCHeaderInterceptors();
  InitializeFormatInterceptors();
}
