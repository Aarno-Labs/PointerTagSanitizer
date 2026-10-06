//===-- ptsan_musl_interceptors.cpp ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The normal PTSan runtime intercepts libc so it can strip pointer IDs before
// entering uninstrumented code.  A libc built with PTSan needs the opposite:
// pointers must retain their IDs so libc can validate and propagate them.
//
// This file contains the musl API boundaries needed by instrumented musl.  The
// initial static implementation mostly uses the linker's --wrap support;
// memcpy/memmove/memset references are redirected by the PTSan pass instead.
// This avoids dlsym-based interception and its dependency on a dynamic loader.
// Raw musl syscall boundaries live in ptsan_musl_syscalls.cpp.
//
//===----------------------------------------------------------------------===//

#include "../ptsan.h"
#include "../ptsan_bounds.h"
#include "../ptsan_ptr_utils.h"
#include "../ptsan_thread.h"
#include "../ptsan_musl_internal.h"

#include "sanitizer_common/sanitizer_linux.h"

#include <elf.h>
#include <link.h>
#include <locale.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/types.h>

using namespace __ptsan;
using namespace __sanitizer;

SANITIZER_INTERFACE_ATTRIBUTE THREADLOCAL u32 ptsan_in_libc = 0;

extern "C" SANITIZER_INTERFACE_ATTRIBUTE void ptsan_enable_libc_instr() {
  ptsan_in_libc = 0;
}

namespace __ptsan {
void InitializeInterceptors() {}
} // namespace __ptsan

DECLARE_REAL(int, __libc_start_main, int (*main)(int, char **, char **),
             int argc, char **argv, void (*init)(), void (*fini)(),
             void (*ldso_fini)())
DECLARE_REAL(void *, malloc, uptr size)
DECLARE_REAL(void *, calloc, uptr nmemb, uptr size)
DECLARE_REAL(void *, __libc_malloc, uptr size)
DECLARE_REAL(void *, __libc_calloc, uptr nmemb, uptr size)
DECLARE_REAL(void *, aligned_alloc, uptr alignment, uptr size)
DECLARE_REAL(int, posix_memalign, void **result, uptr alignment, uptr size)
DECLARE_REAL(void *, realloc, void *ptr, uptr size)
DECLARE_REAL(void *, __libc_realloc, void *ptr, uptr size)
DECLARE_REAL(void, free, void *ptr)
DECLARE_REAL(void, __libc_free, void *ptr)
DECLARE_REAL(uptr, malloc_usable_size, void *ptr)
DECLARE_REAL(int, memcmp, const void *left, const void *right, uptr size)
DECLARE_REAL(void *, memchr, const void *source, int character, uptr size)
DECLARE_REAL(void *, memccpy, void *dest, const void *source, int character,
             uptr size)
DECLARE_REAL(uptr, strlen, const char *source)
DECLARE_REAL(int, strcmp, const char *left, const char *right)
DECLARE_REAL(int, strncmp, const char *left, const char *right, uptr size)
DECLARE_REAL(char *, stpcpy, char *dest, const char *source)
DECLARE_REAL(char *, __stpcpy, char *dest, const char *source)
DECLARE_REAL(char *, strchrnul, const char *source, int character)
DECLARE_REAL(char *, __strchrnul, const char *source, int character)
DECLARE_REAL(uptr, strlcpy, char *dest, const char *source, uptr size)
DECLARE_REAL(char *, stpncpy, char *dest, const char *source, uptr size)
DECLARE_REAL(char *, __stpncpy, char *dest, const char *source, uptr size)
DECLARE_REAL(NORETURN void, longjmp, jmp_buf environment, int value)
DECLARE_REAL(NORETURN void, _longjmp, jmp_buf environment, int value)
DECLARE_REAL(NORETURN void, siglongjmp, sigjmp_buf environment, int value)
DECLARE_REAL(int, munmap, void *address, uptr size)
using PhdrCallback = int (*)(struct dl_phdr_info *, size_t, void *);
DECLARE_REAL(int, dl_iterate_phdr, PhdrCallback callback, void *data)
DECLARE_REAL(int, __pthread_create, void *thread, void *attr,
             void *(*callback)(void *), void *arg)
DECLARE_REAL(int, __clone, int (*callback)(void *), void *stack, int flags,
             void *arg, ...)
DECLARE_REAL(void *, __copy_tls, unsigned char *memory)
DECLARE_REAL(NORETURN void, __unmapself, void *base, uptr size)

extern "C" {
extern char *__progname;
extern char *__progname_full;
extern char **__environ;
extern volatile int __thread_list_lock;
extern void *ThreadListLockProxy asm("__thread_list_lock.ptsan.ptr");

void _init() __attribute__((weak));
extern void (*const __init_array_start)(void);
extern void (*const __init_array_end)(void);
void __ptsan_finish_instrumented_libc_init();
}

// These functions are not linker-wrapped. Bind their REAL declarations to
// the raw symbols directly; in particular, do not redirect intrinsic libcalls.
extern "C" {
void *REAL(memcpy)(void *dest, const void *src, uptr size) asm("memcpy");
void *REAL(memmove)(void *dest, const void *src, uptr size) asm("memmove");
void *REAL(memset)(void *dest, int value, uptr size) asm("memset");
}

static int CapturedArgc;
static char **CapturedArgv;
static int (*CapturedMain)(int, char **, char **);
static char **InitialEnvironment;

static int callMain(int argc, char **argv, char **envp) {
  // Musl saved this raw envp before __libc_start_init. Reuse its startup ID,
  // not the current environ, which application constructors may have changed.
  return CapturedMain(argc, argv, copy_id(envp, InitialEnvironment));
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE int __wrap___libc_start_main(
    int (*main)(int, char **, char **), int argc, char **argv, void (*init)(),
    void (*fini)(), void (*ldso_fini)()) {
  // TLS and the ID tables do not exist yet. Keep musl's startup inputs raw:
  // its uninstrumented __init_libc reads argv[0] to initialize program names.
  CapturedArgc = argc;
  CapturedArgv = argv;
  CapturedMain = main;
  return REAL(__libc_start_main)(callMain, argc, argv, init, fini, ldso_fini);
}

static void initializeArgumentStrings() {
  // The kernel supplied writable strings whose lifetime is the whole process.
  // Register them like globals, independently of main's argv-array source ID.
  for (int i = 0; i < CapturedArgc; ++i) {
    char *str = CapturedArgv[i];
    u16 id = ptsan_get_next_global_id(str, internal_strlen(str) + 1);
    CapturedArgv[i] = set_id(str, id);
  }

  // __init_libc already saved raw aliases of argv[0]. The short name points
  // inside the full name, so both must retain that string's ID. With no argv
  // entries musl instead uses AT_EXECFN (or an empty-string fallback).
  u16 nameID;
  if (CapturedArgc > 0) {
    CHECK(__progname_full == clear_id(CapturedArgv[0]));
    nameID = get_id(CapturedArgv[0]);
  } else {
    uptr name_sz = internal_strlen(__progname_full) + 1;
    nameID = ptsan_get_next_global_id(__progname_full, name_sz);
  }
  __progname_full = set_id(__progname_full, nameID);
  __progname = set_id(__progname, nameID);
}

static void initializeEnvironment() {
  // __init_libc saved the kernel's environment array in __environ. Like argv
  // strings, its entries live for the whole process and include a terminator.
  char **env = __environ;
  uptr count = 0;
  for (; env[count]; ++count) {
    char *str = env[count];
    u16 id = ptsan_get_next_global_id(str, internal_strlen(str) + 1);
    env[count] = set_id(str, id);
  }

  // Include the terminating null slot, even for an empty environment. Public
  // environ aliases this same variable, so both names see the tagged array.
  u16 id = ptsan_get_next_global_id(env, (count + 1) * sizeof(char *));
  __environ = set_id(env, id);
  InitialEnvironment = __environ;
}

static void initializeAuxiliaryVector() {
  // The kernel placed key/value pairs after envp's null slot. __init_libc
  // saved this array before IDs existed; register the terminating pair too.
  size_t *auxv = ptsan_musl_get_auxv();
  uptr count = 0;
  uptr phnum = 0;
  uptr phent = 0;
  for (; auxv[count] != AT_NULL; count += 2) {
    if (auxv[count] == AT_PHNUM)
      phnum = auxv[count + 1];
    if (auxv[count] == AT_PHENT)
      phent = auxv[count + 1];
  }

  // Entry order is not significant; collect the table dimensions before
  // registering pointer-valued payloads. All other values remain untouched.
  for (uptr i = 0; i < count; i += 2) {
    void *ptr = (void *)auxv[i + 1];
    uptr size;
    switch (auxv[i]) {
    case AT_EXECFN:
      // With no argv[0], musl uses AT_EXECFN as the full program name.
      // Argument initialization already registered that backing string.
      if (ptr == clear_id(__progname_full)) {
        auxv[i + 1] = (size_t)copy_id(ptr, __progname_full);
        continue;
      }
      [[fallthrough]];
    case AT_PLATFORM:
    case AT_BASE_PLATFORM:
      size = internal_strlen((const char *)ptr) + 1;
      break;
    case AT_RANDOM:
      size = 16;
      break;
    case AT_PHDR:
      size = phnum * phent;
      break;
    default:
      continue;
    }
    u16 id = ptsan_get_next_global_id(ptr, size);
    auxv[i + 1] = (size_t)set_id(ptr, id);
  }
  u16 id = ptsan_get_next_global_id(auxv, (count + 2) * sizeof(size_t));
  ptsan_musl_set_auxv(set_id(auxv, id));
}

static void initializeStartupMemory() {
  initializeArgumentStrings();
  initializeEnvironment();
  initializeAuxiliaryVector();
}

struct PhdrCallbackContext {
  PhdrCallback callback;
  void *data;
};

static int callPhdrCallback(struct dl_phdr_info *info, size_t size, void *data) {
  auto *context = static_cast<PhdrCallbackContext *>(data);
  auto *raw_info = clear_id(info);
  const auto *phdrs = clear_id(raw_info->dlpi_phdr);
  for (uptr i = 0; i < raw_info->dlpi_phnum; ++i) {
    if (phdrs[i].p_type == PT_PHDR) {
      // musl subtracts p_vaddr from AT_PHDR, which contains a pointer ID after
      // startup. dlpi_addr is a numeric load bias: decode the original pointer
      // before subtracting, since arithmetic may corrupt an encoded ID.
      raw_info->dlpi_addr = reinterpret_cast<uptr>(phdrs) - phdrs[i].p_vaddr;
      break;
    }
  }

  // Preserve musl's tagged stack object and pointer fields for checked callers.
  return context->callback(info, size, context->data);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE int
__wrap_dl_iterate_phdr(PhdrCallback callback, void *data) {
  // Shared-constant discovery also calls here during runtime initialization.
  // Forward the callback without allocating IDs or forcing initialization;
  // decoding the still-raw AT_PHDR pointer leaves its address unchanged.
  PhdrCallbackContext context = {callback, data};
  return REAL(dl_iterate_phdr)(callPhdrCallback, &context);
}

// PTSan global proxy constructors use priorities 0 and 1.  Finish runtime
// initialization immediately after them, before ordinary application
// constructors run.
__attribute__((constructor(2))) static void finishPTSanInitialization() {
  // This global is defined in the uninstrumented __init_tls object. Publish
  // its ID after ordinary global constructors have initialized extern proxies.
  void *lock = (void *)&__thread_list_lock;
  ThreadListLockProxy = set_id(
      lock, ptsan_get_next_global_id(lock, sizeof(__thread_list_lock)));
  // __init_tp saved this address before global proxies were initialized.
  // Let instrumented musl install the normal tagged global-locale pointer.
  uselocale(LC_GLOBAL_LOCALE);
  __ptsan_finish_instrumented_libc_init();
}

// musl does not process .preinit_array.  __libc_start_init runs after musl has
// initialized TLS and its own process state, but before it invokes application
// constructors, which is the point at which the PTSan runtime must start.
//
// This is musl's libc_start_init, with the PTSan initialization call added.
// Keep the constructor iteration in sync with musl src/env/__libc_start_main.c.
extern "C" __attribute__((visibility("hidden"))) void __libc_start_init() {
  __ptsan_init();
  void *self = ptsan_musl_self();
  ptsan_musl_init_main_thread(
      set_id(self, ptsan_get_next_global_id(self, ptsan_musl_thread_size())));
  initializeStartupMemory();
  if (_init)
    _init();
  uptr current = (uptr)&__init_array_start;
  const uptr end = (uptr)&__init_array_end;
  for (; current < end; current += sizeof(void (*)()))
    (*(void (**)())current)();
}

static bool bypassAllocatorMetadata() {
  // Do not touch the TLS counter before musl has initialized TLS.
  return !ptsan_initialized || ptsan_init_is_running || ptsan_in_libc;
}

static void *tagAllocation(void *ptr, uptr size) {
  if (!ptr || bypassAllocatorMetadata()) {
    return ptr;
  }

  // Keep the backing mapping's ID alive for allocator metadata. Callers get
  // a separate ID covering only the allocation returned by malloc.
  return tag_with_next_id(clear_id(ptr), size, FREE_TY::MALLOC);
}

static void checkMemoryRange(const void *ptr, uptr size, const char *kind) {
  if (!ptsan_initialized || ptsan_init_is_running) {
    return;
  }
  check_range_and_report(0, ptr, size, kind);
}

typedef void *(*MallocExecutor)(uptr);
typedef void *(*CallocExecutor)(uptr, uptr);
typedef void *(*ReallocExecutor)(void *, uptr);
typedef void (*FreeExecutor)(void *);

static void *wrapMalloc(uptr size, MallocExecutor executor) {
  if (bypassAllocatorMetadata()) {
    return executor(size);
  }

  u32 old_in_libc = ptsan_in_libc++;
  void *ptr = executor(size);
  ptsan_in_libc = old_in_libc;
  return tagAllocation(ptr, size);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE void *__wrap_malloc(uptr size) {
  return wrapMalloc(size, REAL(malloc));
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE void *__wrap___libc_malloc(uptr size) {
  return wrapMalloc(size, REAL(__libc_malloc));
}

static void *wrapCalloc(uptr nmemb, uptr size, CallocExecutor executor) {
  if (bypassAllocatorMetadata()) {
    return executor(nmemb, size);
  }

  u32 old_in_libc = ptsan_in_libc++;
  void *ptr = executor(nmemb, size);
  ptsan_in_libc = old_in_libc;

  uptr total;
  if (__builtin_mul_overflow(nmemb, size, &total)) {
    return ptr;
  }
  return tagAllocation(ptr, total);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE void *__wrap_calloc(uptr nmemb,
                                                             uptr size) {
  return wrapCalloc(nmemb, size, REAL(calloc));
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE void *__wrap___libc_calloc(uptr nmemb,
                                                                    uptr size) {
  return wrapCalloc(nmemb, size, REAL(__libc_calloc));
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE void *
__wrap_aligned_alloc(uptr alignment, uptr size) {
  if (bypassAllocatorMetadata()) {
    return REAL(aligned_alloc)(alignment, size);
  }

  u32 old_in_libc = ptsan_in_libc++;
  void *ptr = REAL(aligned_alloc)(alignment, size);
  ptsan_in_libc = old_in_libc;
  return tagAllocation(ptr, size);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE int
__wrap_posix_memalign(void **result, uptr alignment, uptr size) {
  if (bypassAllocatorMetadata()) {
    return REAL(posix_memalign)(result, alignment, size);
  }

  checkMemoryRange(result, sizeof(void *), "write");

  u32 old_in_libc = ptsan_in_libc++;
  // Musl is instrumented: preserve the caller's ID for its output-slot write.
  int error = REAL(posix_memalign)(result, alignment, size);
  ptsan_in_libc = old_in_libc;
  if (error) {
    return error;
  }

  void **raw_result = (void **)clear_id(result);
  *raw_result = tagAllocation(*raw_result, size);
  return 0;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE uptr
__wrap_malloc_usable_size(void *ptr) {
  // Musl's no-checks implementation strips accesses, not the pointer's ID.
  if (bypassAllocatorMetadata()) {
    return REAL(malloc_usable_size)(ptr);
  }

  ptsan_validate_allocation(ptr, FREE_TY::MALLOC);
  u32 old_in_libc = ptsan_in_libc++;
  uptr size = REAL(malloc_usable_size)(ptr);
  ptsan_in_libc = old_in_libc;
  return size;
}

static void *wrapRealloc(void *ptr, uptr size, ReallocExecutor executor) {
  // Keep the application ID here while musl resizes or replaces the storage.
  // tag_realloc updates its lifetime after the no-checks implementation returns.
  if (bypassAllocatorMetadata()) {
    return executor(clear_id(ptr), size);
  }

  ptsan_validate_allocation(ptr, FREE_TY::MALLOC);
  u32 old_in_libc = ptsan_in_libc++;
  void *new_ptr = executor(clear_id(ptr), size);
  ptsan_in_libc = old_in_libc;
  return tag_realloc(clear_id(new_ptr), ptr, size);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE void *__wrap_realloc(void *ptr,
                                                              uptr size) {
  return wrapRealloc(ptr, size, REAL(realloc));
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE void *
__wrap___libc_realloc(void *ptr, uptr size) {
  return wrapRealloc(ptr, size, REAL(__libc_realloc));
}

static void wrapFree(void *ptr, FreeExecutor executor) {
  // The wrapper retires the application ID; musl's no-checks implementation
  // receives the raw address to release the storage.
  if (!ptr) {
    return;
  }
  if (bypassAllocatorMetadata()) {
    executor(clear_id(ptr));
    return;
  }

  if (ptsan_mark_ptr_freed(ptr, FREE_TY::MALLOC)) {
    u32 old_in_libc = ptsan_in_libc++;
    executor(clear_id(ptr));
    ptsan_in_libc = old_in_libc;
  }
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE void __wrap_free(void *ptr) {
  wrapFree(ptr, REAL(free));
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE void __wrap___libc_free(void *ptr) {
  wrapFree(ptr, REAL(__libc_free));
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE int __wrap_munmap(void *address,
                                                           uptr size) {
  if (ptsan_initialized && !ptsan_init_is_running) {
    ptsan_validate_munmap(address, size);
  }
  // Preserve the mapping ID until the syscall handler retires it on success.
  return REAL(munmap)(address, size);
}

static bool hasTrackedBounds(const void *ptr) {
  const u16 id = get_id(ptr);
  return id != 0 && id != DefaultIDNo;
}

using LongjmpFunction = NORETURN void (*)(jmp_buf, int);

static NORETURN void wrapLongjmp(LongjmpFunction function, jmp_buf environment,
                                 int value) {
  if (ptsan_initialized && hasTrackedBounds(environment)) {
    checkMemoryRange(environment, sizeof(jmp_buf), "read");
  }
  function(clear_id(environment), value);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE NORETURN void
__wrap_longjmp(jmp_buf environment, int value) {
  wrapLongjmp(REAL(longjmp), environment, value);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE NORETURN void
__wrap__longjmp(jmp_buf environment, int value) {
  wrapLongjmp(REAL(_longjmp), environment, value);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE NORETURN void
__wrap_siglongjmp(sigjmp_buf environment, int value) {
  wrapLongjmp(REAL(siglongjmp), environment, value);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE int
__wrap___sigsetjmp_tail(sigjmp_buf environment, int value) {
  // Musl's assembly passes a raw buffer here on both returns. Keep this
  // internal mask transfer unchecked, using the same raw syscall as musl.
  auto *mask = (__sanitizer_sigset_t *)environment->__ss;
  internal_sigprocmask(SIG_SETMASK, value ? mask : nullptr,
                      value ? nullptr : mask);
  return value;
}

// No-checks callers own any necessary validation. Preserve the destination
// representation on return while passing raw addresses to musl's assembly.
extern "C" SANITIZER_INTERFACE_ATTRIBUTE void *
ptsan_strip_memcpy(void *dest, const void *src, uptr size) {
  REAL(memcpy)(clear_id(dest), clear_id(src), size);
  return dest;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE void *
ptsan_strip_memmove(void *dest, const void *src, uptr size) {
  REAL(memmove)(clear_id(dest), clear_id(src), size);
  return dest;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE void *
ptsan_strip_memset(void *dest, int value, uptr size) {
  REAL(memset)(clear_id(dest), value, size);
  return dest;
}

// musl's architecture-specific memory primitives are assembly and therefore
// cannot be PTSan-instrumented.  Keep these three narrow boundary wrappers even
// when the rest of libc is instrumented.
extern "C" SANITIZER_INTERFACE_ATTRIBUTE void *
__wrap_memcpy(void *dest, const void *src, uptr size) {
  checkMemoryRange(dest, size, "write");
  checkMemoryRange(src, size, "read");
  REAL(memcpy)(clear_id(dest), clear_id(src), size);
  return dest;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE void *
__wrap_memmove(void *dest, const void *src, uptr size) {
  checkMemoryRange(dest, size, "write");
  checkMemoryRange(src, size, "read");
  REAL(memmove)(clear_id(dest), clear_id(src), size);
  return dest;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE void *
__wrap_memset(void *dest, int value, uptr size) {
  checkMemoryRange(dest, size, "write");
  REAL(memset)(clear_id(dest), value, size);
  return dest;
}

// Cache both objects' remaining bounds once. The bounds only ensure that PTSan
// reports before an invalid byte is read; they never change the comparison's
// result or termination point.
extern "C" SANITIZER_INTERFACE_ATTRIBUTE int
__wrap_memcmp(const void *left, const void *right, uptr size) {
  if (!ptsan_initialized) {
    return REAL(memcmp)(clear_id(left), clear_id(right), size);
  }

  const uptr left_bytes = bytes_remaining(left);
  const uptr right_bytes = bytes_remaining(right);
  if (size > left_bytes) {
    report_scan_read(left, left_bytes);
  }
  if (size > right_bytes) {
    report_scan_read(right, right_bytes);
  }

  const unsigned char *raw_left = (const unsigned char *)clear_id(left);
  const unsigned char *raw_right = (const unsigned char *)clear_id(right);
  for (uptr index = 0; index < size; ++index) {
    if (raw_left[index] != raw_right[index]) {
      return (int)raw_left[index] - (int)raw_right[index];
    }
  }
  return 0;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE int __wrap_strcmp(const char *left,
                                                           const char *right) {
  if (!ptsan_initialized) {
    return REAL(strcmp)(clear_id(left), clear_id(right));
  }

  const unsigned char *raw_left = (const unsigned char *)clear_id(left);
  const unsigned char *raw_right = (const unsigned char *)clear_id(right);
  const uptr left_bytes = bytes_remaining(left);
  const uptr right_bytes = bytes_remaining(right);

  for (uptr index = 0;; ++index) {
    if (index >= left_bytes) {
      report_scan_read(left, index);
    }
    if (index >= right_bytes) {
      report_scan_read(right, index);
    }

    const unsigned char left_value = raw_left[index];
    const unsigned char right_value = raw_right[index];
    if (left_value != right_value || left_value == '\0') {
      return (int)left_value - (int)right_value;
    }
  }
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE int
__wrap_strncmp(const char *left, const char *right, uptr size) {
  if (!ptsan_initialized) {
    return REAL(strncmp)(clear_id(left), clear_id(right), size);
  }

  const unsigned char *raw_left = (const unsigned char *)clear_id(left);
  const unsigned char *raw_right = (const unsigned char *)clear_id(right);
  const uptr left_bytes = bytes_remaining(left);
  const uptr right_bytes = bytes_remaining(right);

  for (uptr index = 0; index < size; ++index) {
    if (index >= left_bytes) {
      report_scan_read(left, index);
    }
    if (index >= right_bytes) {
      report_scan_read(right, index);
    }

    const unsigned char left_value = raw_left[index];
    const unsigned char right_value = raw_right[index];
    if (left_value != right_value || left_value == '\0') {
      return (int)left_value - (int)right_value;
    }
  }
  return 0;
}

// These musl routines deliberately use word-at-a-time reads that may cross an
// object's logical access range. Validate the logical operation here, then
// keep those implementation reads outside PTSan's object model.
extern "C" SANITIZER_INTERFACE_ATTRIBUTE uptr
__wrap_strlen(const char *source) {
  if (!ptsan_initialized) {
    return REAL(strlen)(clear_id(source));
  }
  return find_cstring_character(source, '\0');
}

using StpcpyFunction = char *(*)(char *, const char *);

static char *wrapStpcpy(StpcpyFunction function, char *dest,
                        const char *source) {
  // The no-checks body handles word overreads; preserve IDs through the copy
  // and on its returned destination pointer.
  if (!ptsan_initialized) {
    return function(dest, source);
  }

  const uptr length = find_cstring_character(source, 0);
  checkMemoryRange(dest, length + 1, "write");
  return function(dest, source);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE char *
__wrap_stpcpy(char *dest, const char *source) {
  return wrapStpcpy(REAL(stpcpy), dest, source);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE char *
__wrap___stpcpy(char *dest, const char *source) {
  return wrapStpcpy(REAL(__stpcpy), dest, source);
}

using StrchrnulFunction = char *(*)(const char *, int);

static char *wrapStrchrnul(StrchrnulFunction function, const char *source,
                           int character) {
  if (!ptsan_initialized) {
    return function(clear_id(source), character);
  }

  const uptr offset = find_cstring_character(source, character);
  return set_id((char *)(get_addr(source) + offset), get_id(source));
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE char *
__wrap_strchrnul(const char *source, int character) {
  return wrapStrchrnul(REAL(strchrnul), source, character);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE char *
__wrap___strchrnul(const char *source, int character) {
  return wrapStrchrnul(REAL(__strchrnul), source, character);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE uptr __wrap_strlcpy(char *dest,
                                                             const char *source,
                                                             uptr size) {
  // Preserve IDs in the no-checks body, including its call to checked strlen.
  if (!ptsan_initialized) {
    return REAL(strlcpy)(dest, source, size);
  }
  const uptr source_length = find_cstring_character(source, 0);
  const uptr write_size = size < source_length + 1 ? size : source_length + 1;
  checkMemoryRange(dest, write_size, "write");
  return REAL(strlcpy)(dest, source, size);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE void *
__wrap_memchr(const void *source, int character, uptr size) {
  if (!ptsan_initialized) {
    return REAL(memchr)(clear_id(source), character, size);
  }

  const uptr offset = find_bounded_character(source, character, size);
  if (offset == size) {
    return nullptr;
  }
  return set_id((void *)(get_addr(source) + offset), get_id(source));
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE void *
__wrap_memccpy(void *dest, const void *source, int character, uptr size) {
  // The no-checks body may read whole words past the matching byte. It still
  // preserves the destination ID on a non-null result.
  if (!ptsan_initialized) {
    return REAL(memccpy)(dest, source, character, size);
  }

  const uptr offset = find_bounded_character(source, character, size);
  const uptr copy_size = offset == size ? size : offset + 1;
  checkMemoryRange(dest, copy_size, "write");
  return REAL(memccpy)(dest, source, character, size);
}

using StpncpyFunction = char *(*)(char *, const char *, uptr);

static char *wrapStpncpy(StpncpyFunction function, char *dest,
                         const char *source, uptr size) {
  // Preserve IDs through the no-checks copy. Its padding memset uses the
  // no-checks memory wrapper; the full destination range is checked here.
  if (!ptsan_initialized) {
    return function(dest, source, size);
  }

  find_bounded_character(source, '\0', size);
  checkMemoryRange(dest, size, "write");
  return function(dest, source, size);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE char *
__wrap_stpncpy(char *dest, const char *source, uptr size) {
  return wrapStpncpy(REAL(stpncpy), dest, source, size);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE char *
__wrap___stpncpy(char *dest, const char *source, uptr size) {
  return wrapStpncpy(REAL(__stpncpy), dest, source, size);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE pid_t __wrap_vfork() {
  Report("PTSan does not support vfork process creation\n");
  internal__exit(1);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE int
__wrap___clone(int (*callback)(void *), void *stack, int flags, void *arg,
               ...) {
  // Pinned musl uses this exact flag set beneath pthread_create. Match it
  // exactly so a changed thread-creation contract fails closed.
  const int musl_pthread_flags = CLONE_VM | CLONE_FS | CLONE_FILES |
                                 CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM |
                                 CLONE_SETTLS | CLONE_PARENT_SETTID |
                                 CLONE_CHILD_CLEARTID | CLONE_DETACHED;
  const bool is_musl_pthread = flags == musl_pthread_flags;

  // Public clone and posix_spawn may use the same restricted execution model
  // as vfork: shared memory, with the parent suspended until child exit/exec.
  const bool is_vfork_like =
      (flags & (CLONE_VM | CLONE_VFORK | CLONE_THREAD)) ==
      (CLONE_VM | CLONE_VFORK);

  if (!(flags & CLONE_VM)) {
    Report("PTSan does not support fork-like process creation\n");
    internal__exit(1);
  }
  if (!is_musl_pthread && !is_vfork_like) {
    Report("PTSan does not support this shared-memory clone mode\n");
    internal__exit(1);
  }

  void *parent_tid = nullptr;
  void *tls = nullptr;
  void *child_tid = nullptr;

  // Some internal musl callers pass only the four required arguments. Decode
  // only as far as the flags say the caller populated the optional positions.
  va_list arguments;
  va_start(arguments, arg);
  if (flags & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) {
    parent_tid = va_arg(arguments, void *);
    tls = va_arg(arguments, void *);
    child_tid = va_arg(arguments, void *);
  } else if (flags & CLONE_SETTLS) {
    parent_tid = va_arg(arguments, void *);
    tls = va_arg(arguments, void *);
  } else if (flags & (CLONE_PIDFD | CLONE_PARENT_SETTID)) {
    parent_tid = va_arg(arguments, void *);
  }
  va_end(arguments);

  if (ptsan_initialized) {
    // musl clone assembly stores arg on x86-64, and callback plus arg on
    // AArch64, below the aligned stack pointer.
    const uptr aligned_stack = get_addr(stack) & ~(uptr)15;
#if defined(__aarch64__)
    constexpr uptr saved_bytes = 2 * sizeof(void *);
#elif defined(__x86_64__)
    constexpr uptr saved_bytes = sizeof(void *);
#else
#error "Unsupported architecture for the PTSan musl clone boundary"
#endif
    void *argument_slot =
        set_id((void *)(aligned_stack - saved_bytes), get_id(stack));
    checkMemoryRange(argument_slot, saved_bytes, "write");

    if (flags & (CLONE_PIDFD | CLONE_PARENT_SETTID)) {
      checkMemoryRange(parent_tid, sizeof(pid_t), "write");
    }
    if (flags & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) {
      checkMemoryRange(child_tid, sizeof(pid_t), "write");
    }
  }

  if (is_musl_pthread) {
    ptsan_musl_init_child_thread(clear_id(tls), tls);
  }

  stack = clear_id(stack);
  parent_tid = clear_id(parent_tid);
  tls = clear_id(tls);
  child_tid = clear_id(child_tid);

  // arg is an opaque cookie delivered to instrumented code in the child, so
  // preserve its ID across this assembly boundary.
  return REAL(__clone)(callback, stack, flags, arg, parent_tid, tls, child_tid);
}

// __copy_tls shares an uninstrumented object with musl's pre-TLS startup code.
// The startup call is local to that object; linker wrapping catches the later
// cross-object call from pthread_create. Keep the backing allocation's ID;
// __clone restores the raw ABI self slot before starting the child.
extern "C" SANITIZER_INTERFACE_ATTRIBUTE void *
__wrap___copy_tls(unsigned char *memory) {
  return copy_id(REAL(__copy_tls)(clear_id(memory)), memory);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE NORETURN void
__wrap___unmapself(void *base, uptr size) {
  // Detached threads unmap their own stack and exit directly in assembly.
  // There is no return path on which to retire the backing allocation's ID.
  FinishMuslThread();
  ptsan_retire_mmap_if_exact(base, size);
  REAL(__unmapself)(clear_id(base), size);
}

static PTSanThread *PTSanThreadInit(void *arg, bool c11) {
  ptsan_in_libc++;
  PTSanThread *thread = (PTSanThread *)arg;
  thread->Init();
  SetCurrentThread(thread);
  thread->InitTLS();
  // musl's C11 trampoline leaves application signals blocked.
  if (!c11)
    SetSigProcMask(&thread->starting_sigset_, nullptr);
  if (ClLAM)
    ptsan_enable_lam_u48();
  ptsan_in_libc--;
  return thread;
}

static void *PTSanThreadStartFunc(void *arg) {
  return PTSanThreadInit(arg, false)->ThreadStart();
}

static int PTSanThreadStartC11Func(void *arg) {
  return PTSanThreadInit(arg, true)->ThreadStartC11();
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE int
__wrap___pthread_create(void *thread, void *attr, void *(*callback)(void *),
                       void *arg) {
  ptsan_in_libc++;

  // AdjustStackSize uses dl_iterate_phdr.  Its callback is part of this
  // uninstrumented runtime, while musl passes it a pointer to an instrumented
  // stack object.  PTSan's TLS is small, so musl's normal stack sizing is
  // sufficient and avoids that callback boundary.
  PTSanThread *ptsan_thread = PTSanThread::Create(callback, arg);
  if (ptsan_musl_is_c11_thread(attr)) {
    // musl casts this back to int (*)(void *) in start_c11. It also blocks
    // application signals before clone, so no additional mask is needed.
    int result = REAL(__pthread_create)(
        thread, attr,
        reinterpret_cast<void *(*)(void *)>(PTSanThreadStartC11Func),
        ptsan_thread);
    if (result != 0)
      ptsan_thread->Release();
    ptsan_in_libc--;
    return result;
  }

  ScopedBlockSignals block(&ptsan_thread->starting_sigset_);
  int result = REAL(__pthread_create)(thread, attr, PTSanThreadStartFunc,
                                     ptsan_thread);
  if (result != 0)
    ptsan_thread->Release();
  ptsan_in_libc--;
  return result;
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE int
__wrap_pthread_create(void *thread, void *attr, void *(*callback)(void *),
                     void *arg) {
  // pthread_create is an alias, not a call to __pthread_create. Wrap both
  // names so pthread and C11 callers share the same initialization.
  return __wrap___pthread_create(thread, attr, callback, arg);
}
