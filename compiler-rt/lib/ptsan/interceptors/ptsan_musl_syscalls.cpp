//===-- ptsan_musl_syscalls.cpp ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Linux syscall handling selected while building musl with PTSan's syscall.h
// overlay.  One fixed-width boundary dispatches to typed handlers, which
// validate and strip pointer arguments before calling the supplied raw or
// cancellable executor while retaining musl's raw syscall result semantics.
//
//===----------------------------------------------------------------------===//

#include "../ptsan.h"
#include "../ptsan_bounds.h"
#include "../ptsan_ptr_utils.h"
#include "../ptsan_thread.h"

#include "sanitizer_common/sanitizer_allocator_internal.h"
#include "sanitizer_common/sanitizer_libc.h"
#include "sanitizer_common/sanitizer_mutex.h"
#include "sanitizer_common/sanitizer_platform_limits_posix.h"
#if defined(__x86_64__)
#include <asm/prctl.h>

// Keep arch_prctl handling independent of the build host's header version.
// These command values are part of the Linux x86 userspace ABI.
#ifndef ARCH_GET_XCOMP_SUPP
#define ARCH_GET_XCOMP_SUPP 0x1021
#endif
#ifndef ARCH_GET_XCOMP_PERM
#define ARCH_GET_XCOMP_PERM 0x1022
#endif
#ifndef ARCH_REQ_XCOMP_PERM
#define ARCH_REQ_XCOMP_PERM 0x1023
#endif
#ifndef ARCH_GET_XCOMP_GUEST_PERM
#define ARCH_GET_XCOMP_GUEST_PERM 0x1024
#endif
#ifndef ARCH_REQ_XCOMP_GUEST_PERM
#define ARCH_REQ_XCOMP_GUEST_PERM 0x1025
#endif

#ifndef ARCH_GET_UNTAG_MASK
#define ARCH_GET_UNTAG_MASK 0x4001
#endif
#ifndef ARCH_ENABLE_TAGGED_ADDR
#define ARCH_ENABLE_TAGGED_ADDR 0x4002
#endif
#ifndef ARCH_GET_MAX_TAG_BITS
#define ARCH_GET_MAX_TAG_BITS 0x4003
#endif
#ifndef ARCH_FORCE_TAGGED_SVA
#define ARCH_FORCE_TAGGED_SVA 0x4004
#endif

#ifndef ARCH_SHSTK_ENABLE
#define ARCH_SHSTK_ENABLE 0x5001
#endif
#ifndef ARCH_SHSTK_DISABLE
#define ARCH_SHSTK_DISABLE 0x5002
#endif
#ifndef ARCH_SHSTK_LOCK
#define ARCH_SHSTK_LOCK 0x5003
#endif
#ifndef ARCH_SHSTK_UNLOCK
#define ARCH_SHSTK_UNLOCK 0x5004
#endif
#ifndef ARCH_SHSTK_STATUS
#define ARCH_SHSTK_STATUS 0x5005
#endif
#endif
#include <linux/futex.h>

#include <fcntl.h>
#include <mqueue.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <sys/fanotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/msg.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <termios.h>
#include <time.h>

#if !defined(__x86_64__) && !defined(__aarch64__)
#error                                                                         \
    "PTSan instrumented-musl syscall boundaries currently support x86-64 and AArch64"
#endif

struct itimerval;
struct itimerspec;
struct linux_dirent64;
struct mq_attr;
struct rlimit;
struct robust_list_head;
struct sockaddr;
struct stat;
struct statfs;
struct timespec;
struct timeval;
struct timezone;
struct timex;
struct tms;

using namespace __ptsan;
using namespace __sanitizer;

//===----------------------------------------------------------------------===//
// Shared checking and dispatch utilities
//===----------------------------------------------------------------------===//

// Interceptor checking invariants:
// - PTSan is a practical security tool, not a general undefined-behavior
//   detector. Do not add checks merely to diagnose arbitrary overreads. Model
//   an overread when attacker-controlled addressing could expose memory outside
//   the object the program intended to make accessible.
// - Check caller-provided memory before the interceptor reads it.
// - After reporting an invalid range, the interceptor may still read memory
//   that libc or the kernel would have read anyway.
// - A check result must never affect syscall arguments, execution, or results.
//   It may only suppress further validation of an already-invalid range.
// - In a loop over one object, cache its bounds and compare locally instead of
//   calling the general-purpose range checker for every element.
static bool checkMemoryRange(const void *ptr, uptr size) {
  if (check_range(ptr, size)) {
    return true;
  }
  ptsan_complain(0, 0xdeadbeef, get_id(ptr), "libc", get_addr(ptr), size);
  return false;
}

static uptr bytesRemainingInObject(const void *ptr) {
  const u16 id = get_id(ptr);
  const uptr base = ptsan_bounds[id].base;
  const uptr span = ptsan_bounds[id].span;
  const uptr offset = (uptr)ptr - base;
  if (offset > span) {
    return 0;
  }
  return span - offset;
}

// Cache the string's bounds, report before the first out-of-bounds byte read,
// and continue until the actual terminator that libc or the kernel would read.
static const char *checkAndStripCString(const char *ptr,
                                        bool optional = false) {
  if (optional && !ptr) {
    return ptr;
  }
  const char *source = clear_id(ptr);
  const u16 string_id = get_id(ptr);
  const uptr valid_bytes = bytesRemainingInObject(ptr);
  uptr index = 0;
  do {
    if (index == valid_bytes) {
      ptsan_complain(0, 0xdeadbeef, string_id, "libc", get_addr(ptr + index),
                     1);
    }
  } while (source[index++]);
  return source;
}

// Some kernel string interfaces read no more than a fixed number of bytes and
// accept a nonterminated buffer of exactly that size.
static const char *checkAndStripBoundedCString(const char *ptr,
                                               uptr maximum_size) {
  const char *source = clear_id(ptr);
  const u16 string_id = get_id(ptr);
  const uptr valid_bytes = bytesRemainingInObject(ptr);

  for (uptr index = 0; index != maximum_size; ++index) {
    if (index == valid_bytes) {
      ptsan_complain(0, 0xdeadbeef, string_id, "libc", get_addr(ptr + index),
                     1);
    }
    if (!source[index]) {
      break;
    }
  }
  return source;
}

template <typename T>
static T checkAndStrip(T ptr, uptr size, bool optional = false) {
  if (optional && !ptr) {
    return ptr;
  }
  checkMemoryRange(ptr, size);
  return clear_id(ptr);
}

struct SyscallExecutor {
  virtual sptr operator()(sptr number, sptr a, sptr b, sptr c, sptr d, sptr e,
                          sptr f) const = 0;
};

#define SYSCALL_HANDLER(name) ptsan_musl_##name

//===----------------------------------------------------------------------===//
// Common direct-buffer and virtual-memory handlers
//===----------------------------------------------------------------------===//

static sptr SYSCALL_HANDLER(uname)(const SyscallExecutor &executor,
                                   void *buffer) {
  buffer = checkAndStrip(buffer, sizeof(struct utsname));
  const sptr result = executor(SYS_uname, (sptr)buffer, 0, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(write)(const SyscallExecutor &executor, int fd,
                                   const void *buffer, uptr size) {
  buffer = checkAndStrip(buffer, size);
  const sptr result =
      executor(SYS_write, (sptr)fd, (sptr)buffer, (sptr)size, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(read)(const SyscallExecutor &executor, int fd,
                                  void *buffer, uptr size) {
  buffer = checkAndStrip(buffer, size);
  const sptr result =
      executor(SYS_read, (sptr)fd, (sptr)buffer, (sptr)size, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(pread64)(const SyscallExecutor &executor, int fd,
                                     void *buffer, uptr size, sptr offset) {
  buffer = checkAndStrip(buffer, size);
  const sptr result =
      executor(SYS_pread64, (sptr)fd, (sptr)buffer, (sptr)size, offset, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(pwrite64)(const SyscallExecutor &executor, int fd,
                                      const void *buffer, uptr size,
                                      sptr offset) {
  buffer = checkAndStrip(buffer, size);
  const sptr result =
      executor(SYS_pwrite64, (sptr)fd, (sptr)buffer, (sptr)size, offset, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(getrandom)(const SyscallExecutor &executor,
                                       void *buffer,
                                       uptr size, unsigned flags) {
  buffer = checkAndStrip(buffer, size);
  const sptr result =
      executor(SYS_getrandom, (sptr)buffer, (sptr)size, (sptr)flags, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(brk)(const SyscallExecutor &executor,
                                void *requested_break) {
  static StaticSpinMutex lock;
  static uptr base;
  static u16 id;
  SpinMutexLock guard(&lock);

  // Track growth from the first observed break. Give even the initial query
  // an ID: mallocng derives pointers from that saved address, not from the
  // return value of the subsequent growth call.
  if (!base) {
    base = executor(SYS_brk, 0, 0, 0, 0, 0, 0);
    id = ptsan_get_next_global_id((void *)base, 0);
  }

  const uptr result = executor(SYS_brk, (sptr)clear_id(requested_break),
                               0, 0, 0, 0, 0);
  // Linux returns the actual break, including when a request fails. Keep
  // the ID stable and adjust its span rather than allocating a new ID.
  // A shrink below our initial observation leaves no tracked bytes live.
#ifndef PTSAN_NO_IDS
  ptsan_bounds[id].span = result > base ? result - base : 0;
#endif
  return (sptr)set_id((void *)result, id);
}

static sptr SYSCALL_HANDLER(mmap)(const SyscallExecutor &executor,
                                  void *requested_address, uptr size,
                                  int protection, int flags, int fd,
                                  s64 offset) {
  const sptr result =
      executor(SYS_mmap, (sptr)clear_id(requested_address), (sptr)size,
               (sptr)protection, (sptr)flags, (sptr)fd, (sptr)offset);

  // Backing mappings have independent IDs even when malloc will subsequently
  // assign a narrower ID to an allocation within this memory. The mapping
  // covers whole pages; the malloc ID still covers only the requested object.
  if (!result || internal_iserror((uptr)result)) {
    return result;
  }
  u16 id = ptsan_get_next_nonstack_id(
      (void *)result, RoundUpTo(size, GetPageSizeCached()), FREE_TY::MMAP);
  return (sptr)set_id((void *)result, id);
}

static sptr SYSCALL_HANDLER(munmap)(const SyscallExecutor &executor,
                                    void *address,
                                    uptr size) {
  const sptr result =
      executor(SYS_munmap, (sptr)clear_id(address), (sptr)size, 0, 0, 0, 0);
  if (result == 0) {
    // Hidden musl calls and direct syscall() use the same raw boundary. Allow
    // both to retain kernel behavior, while still retiring exact mappings
    // when the caller supplied enough metadata to identify one.
    ptsan_retire_mmap_if_exact(address, size);
  }
  return result;
}

static sptr SYSCALL_HANDLER(mremap)(const SyscallExecutor &executor,
                                    void *old_address,
                                    uptr old_size, uptr new_size, int flags,
                                    void *new_address) {
  if (!old_size || !new_size || (flags != 0 && flags != MREMAP_MAYMOVE)) {
    Report("PTSan does not support this mremap operation\n");
    internal__exit(1);
  }

  const u16 old_id = get_id(old_address);
  if (!old_id || old_id == DefaultIDNo) {
    // Musl also uses mremap on untracked process mappings while discovering
    // the main-thread stack. Leave those results untagged, but preserve the
    // default ID after exhaustion so a later munmap accepts the mapping.
    const sptr result =
        executor(SYS_mremap, (sptr)clear_id(old_address), (sptr)old_size,
                 (sptr)new_size, (sptr)flags, (sptr)clear_id(new_address), 0);
    if (internal_iserror((uptr)result)) {
      return result;
    }
    return (sptr)set_id((void *)result, old_id);
  }

  if (ptsan_obj_types[old_id] != FREE_TY::MMAP ||
      ptsan_bounds[old_id].base != (u64)old_address ||
      ptsan_bounds[old_id].span != RoundUpTo(old_size, GetPageSizeCached())) {
    Report("PTSan rejected unsupported mremap before syscall: the old range "
           "must exactly match one tracked mmap allocation\n");
    internal__exit(1);
  }

  const sptr result =
      executor(SYS_mremap, (sptr)clear_id(old_address), (sptr)old_size,
               (sptr)new_size, (sptr)flags, (sptr)clear_id(new_address), 0);
  if (internal_iserror((uptr)result)) {
    return result;
  }

  // Match realloc: an in-place resize preserves the allocation ID, while a
  // move retires it so every pointer into the old location becomes stale.
  const uptr mapped_size = RoundUpTo(new_size, GetPageSizeCached());
  if ((void *)result == clear_id(old_address)) {
    ptsan_bounds[old_id].span = mapped_size;
    return (sptr)old_address;
  }

  ptsan_mark_ptr_freed(old_address, FREE_TY::MMAP);
  const u16 new_id =
      ptsan_get_next_nonstack_id((void *)result, mapped_size, FREE_TY::MMAP);
  return (sptr)set_id((void *)result, new_id);
}

static sptr SYSCALL_HANDLER(shmat)(const SyscallExecutor &executor,
                                   int shared_memory_id,
                                   const void *requested_address, int flags) {
  if (flags & SHM_REMAP) {
    Report("PTSan does not support SHM_REMAP attachments\n");
    internal__exit(1);
  }

  // The attachment syscall does not carry its size. Query the segment's
  // immutable public size before attaching, using the raw executor to avoid
  // recursively entering the dispatcher.
  struct shmid_ds descriptor = {};
  const sptr descriptor_result =
      executor(SYS_shmctl, (sptr)shared_memory_id, (sptr)IPC_STAT,
               (sptr)&descriptor, 0, 0, 0);

  const sptr result =
      executor(SYS_shmat, (sptr)shared_memory_id,
               (sptr)clear_id(requested_address), (sptr)flags, 0, 0, 0);
  if (internal_iserror((uptr)result)) {
    return result;
  }
  if (internal_iserror((uptr)descriptor_result)) {
    Report("PTSan could not determine the size of a successful shmat "
           "attachment\n");
    internal__exit(1);
  }

  const u16 id = ptsan_get_next_nonstack_id(
      (void *)result, descriptor.shm_segsz, FREE_TY::MMAP);
  return (sptr)set_id((void *)result, id);
}

static sptr SYSCALL_HANDLER(shmdt)(const SyscallExecutor &executor,
                                   const void *address) {
#ifdef PTSAN_NO_IDS
  return executor(SYS_shmdt, (sptr)clear_id(address), 0, 0, 0, 0, 0);
#else
  const u16 id = get_id(address);
  if (id == DefaultIDNo) {
    return executor(SYS_shmdt, (sptr)clear_id(address), 0, 0, 0, 0, 0);
  }
  if (!id) {
    Report("PTSan does not support shmdt with an untracked address\n");
    internal__exit(1);
  }
  if (ptsan_obj_types[id] != FREE_TY::MMAP ||
      ptsan_bounds[id].base != (u64)address) {
    Report("PTSan rejected unsupported shmdt before syscall: the address must "
           "be the base of one tracked mapping\n");
    internal__exit(1);
  }

  const sptr result =
      executor(SYS_shmdt, (sptr)clear_id(address), 0, 0, 0, 0, 0);
  if (result == 0) {
    ptsan_mark_ptr_freed((void *)address, FREE_TY::MMAP);
  }
  return result;
#endif
}

static sptr SYSCALL_HANDLER(clone)(const SyscallExecutor &executor,
                                  sptr a, sptr b, sptr c, sptr d, sptr e, sptr f) {
#if defined(__aarch64__)
  // AArch64 has no fork syscall. musl _Fork uses this exact clone form;
  // preserve the existing fork-then-exec support without admitting raw threads.
  if (a == SIGCHLD && b == 0 && c == 0 && d == 0 && e == 0 && f == 0)
    return executor(SYS_clone, a, b, c, d, e, f);
#endif
  Report("PTSan does not support raw clone process creation\n");
  internal__exit(1);
}

// Allow ordinary fork so the child can replace its copied runtime state with
// exec. Continuing instrumented execution in the child is not yet guaranteed
// when fork has discarded other threads and their PTSan state.
#ifdef SYS_fork
static sptr SYSCALL_HANDLER(fork)(const SyscallExecutor &executor) {
  return executor(SYS_fork, 0, 0, 0, 0, 0, 0);
}
#endif

// The kernel retains this nested stack pointer for later signal delivery.
static sptr SYSCALL_HANDLER(sigaltstack)(const SyscallExecutor &) {
  Report("PTSan does not support alternate signal stacks\n");
  internal__exit(1);
}

#ifdef SYS_vfork
static sptr SYSCALL_HANDLER(vfork)(const SyscallExecutor &) {
  Report("PTSan does not support vfork process creation\n");
  internal__exit(1);
}
#endif

// The kernel retains this live, mutating pointer graph until thread exit.
static sptr SYSCALL_HANDLER(set_robust_list)(const SyscallExecutor &) {
  Report("PTSan does not support robust mutex lists\n");
  internal__exit(1);
}

static sptr SYSCALL_HANDLER(semctl)(const SyscallExecutor &) {
  Report("PTSan does not support System V semaphore control\n");
  internal__exit(1);
}

static sptr SYSCALL_HANDLER(quotactl)(const SyscallExecutor &) {
  Report("PTSan does not support filesystem quota control\n");
  internal__exit(1);
}

static sptr SYSCALL_HANDLER(mount)(const SyscallExecutor &) {
  Report("PTSan does not support filesystem mounting\n");
  internal__exit(1);
}

static sptr SYSCALL_HANDLER(ptrace)(const SyscallExecutor &) {
  Report("PTSan does not support process tracing\n");
  internal__exit(1);
}

// prctl's operation determines whether its remaining arguments are scalars or
// pointers. Keep a closed list so newly added Linux operations cannot silently
// bypass pointer handling.
static sptr SYSCALL_HANDLER(prctl)(const SyscallExecutor &executor,
                                   int operation,
                                   unsigned long argument2,
                                   unsigned long argument3,
                                   unsigned long argument4,
                                   unsigned long argument5) {
  switch (operation) {
  case PR_GET_PDEATHSIG:
  case PR_GET_TSC:
  case PR_GET_CHILD_SUBREAPER: {
    int *output = (int *)argument2;
    output = checkAndStrip(output, sizeof(int));
    argument2 = (unsigned long)output;
    break;
  }
  case PR_SET_NAME: {
    const char *name = (const char *)argument2;
    name = checkAndStripBoundedCString(name, sizeof(char[16]));
    argument2 = (unsigned long)name;
    break;
  }
  case PR_GET_NAME: {
    char *name = (char *)argument2;
    name = checkAndStrip(name, sizeof(char[16]));
    argument2 = (unsigned long)name;
    break;
  }
  case PR_SET_PDEATHSIG:
  case PR_GET_DUMPABLE:
  case PR_SET_DUMPABLE:
  case PR_SET_UNALIGN:
  case PR_GET_KEEPCAPS:
  case PR_SET_KEEPCAPS:
  case PR_SET_FPEMU:
  case PR_SET_FPEXC:
  case PR_GET_TIMING:
  case PR_SET_TIMING:
  case PR_SET_ENDIAN:
  case PR_GET_SECCOMP:
  case PR_CAPBSET_READ:
  case PR_CAPBSET_DROP:
  case PR_SET_TSC:
  case PR_GET_SECUREBITS:
  case PR_SET_SECUREBITS:
  case PR_SET_TIMERSLACK:
  case PR_GET_TIMERSLACK:
  case PR_TASK_PERF_EVENTS_DISABLE:
  case PR_TASK_PERF_EVENTS_ENABLE:
  case PR_MCE_KILL:
  case PR_MCE_KILL_GET:
  case PR_SET_PTRACER:
  case PR_SET_CHILD_SUBREAPER:
  case PR_SET_NO_NEW_PRIVS:
  case PR_GET_NO_NEW_PRIVS:
  case PR_SET_THP_DISABLE:
  case PR_GET_THP_DISABLE:
  case PR_MPX_ENABLE_MANAGEMENT:
  case PR_MPX_DISABLE_MANAGEMENT:
  case PR_SET_FP_MODE:
  case PR_GET_FP_MODE:
  case PR_CAP_AMBIENT:
  case PR_SVE_SET_VL:
  case PR_SVE_GET_VL:
  case PR_GET_SPECULATION_CTRL:
  case PR_SET_SPECULATION_CTRL:
  case PR_PAC_RESET_KEYS:
  case PR_SET_TAGGED_ADDR_CTRL:
  case PR_GET_TAGGED_ADDR_CTRL:
  case PR_SET_IO_FLUSHER:
  case PR_GET_IO_FLUSHER:
  case PR_PAC_SET_ENABLED_KEYS:
  case PR_PAC_GET_ENABLED_KEYS:
    break;
  default:
    Report("PTSan does not support prctl operation %d\n", operation);
    internal__exit(1);
  }

  const sptr result =
      executor(SYS_prctl, (sptr)operation, (sptr)argument2, (sptr)argument3,
               (sptr)argument4, (sptr)argument5, 0);
  return result;
}

// These operations describe virtual-memory ranges rather than ranges within
// one PTSan object. Their lengths may legitimately cross object or mapping
// boundaries, so strip the starting address without applying an object-range
// check. None of them creates or retires allocation metadata.

static sptr SYSCALL_HANDLER(madvise)(const SyscallExecutor &executor,
                                     void *address,
                                     size_t length, int advice) {
  address = clear_id(address);
  const sptr result =
      executor(SYS_madvise, (sptr)address, (sptr)length, (sptr)advice, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(mlock)(const SyscallExecutor &executor,
                                   const void *address, size_t length) {
  address = clear_id(address);
  const sptr result =
      executor(SYS_mlock, (sptr)address, (sptr)length, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(mlock2)(const SyscallExecutor &executor,
                                    const void *address, size_t length,
                                    unsigned flags) {
  address = clear_id(address);
  const sptr result =
      executor(SYS_mlock2, (sptr)address, (sptr)length, (sptr)flags, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(mprotect)(const SyscallExecutor &executor,
                                      void *address,
                                      size_t length, int protection) {
  address = clear_id(address);
  const sptr result = executor(SYS_mprotect, (sptr)address, (sptr)length,
                               (sptr)protection, 0, 0, 0);
  if (result == 0) {
    ptsan_check_mprotect(address, length, protection);
  }
  return result;
}

static sptr SYSCALL_HANDLER(pkey_mprotect)(const SyscallExecutor &executor,
                                           void *address, size_t length,
                                           int protection, int key) {
  address = clear_id(address);
  const sptr result = executor(SYS_pkey_mprotect, (sptr)address, (sptr)length,
                               (sptr)protection, (sptr)key, 0, 0);
  if (result == 0) {
    ptsan_check_mprotect(address, length, protection);
  }
  return result;
}

static sptr SYSCALL_HANDLER(msync)(const SyscallExecutor &executor,
                                   void *address,
                                   size_t length, int flags) {
  address = clear_id(address);
  const sptr result =
      executor(SYS_msync, (sptr)address, (sptr)length, (sptr)flags, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(munlock)(const SyscallExecutor &executor,
                                     const void *address, size_t length) {
  address = clear_id(address);
  const sptr result =
      executor(SYS_munlock, (sptr)address, (sptr)length, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(remap_file_pages)(const SyscallExecutor &executor,
                                              void *address, size_t size,
                                              int protection,
                                              size_t page_offset, int flags) {
  address = clear_id(address);
  const sptr result =
      executor(SYS_remap_file_pages, (sptr)address, (sptr)size,
               (sptr)protection, (sptr)page_offset, (sptr)flags, 0);
  return result;
}

// Architecture and thread syscalls pass addresses which the kernel either
// dereferences immediately or retains as part of the thread ABI.

#ifdef SYS_arch_prctl
static sptr SYSCALL_HANDLER(arch_prctl)(const SyscallExecutor &executor,
                                        int operation,
                                        unsigned long argument) {
  switch (operation) {
  case ARCH_GET_FS:
  case ARCH_GET_GS:
  case ARCH_GET_XCOMP_SUPP:
  case ARCH_GET_XCOMP_PERM:
  case ARCH_GET_XCOMP_GUEST_PERM:
  case ARCH_GET_UNTAG_MASK:
  case ARCH_GET_MAX_TAG_BITS:
  case ARCH_SHSTK_STATUS: {
    unsigned long *output = (unsigned long *)argument;
    output = checkAndStrip(output, sizeof(unsigned long));
    argument = (unsigned long)output;
    break;
  }
  case ARCH_SET_FS:
  case ARCH_SET_GS:
  case ARCH_MAP_VDSO_X32:
  case ARCH_MAP_VDSO_32:
  case ARCH_MAP_VDSO_64:
    argument = (unsigned long)clear_id((void *)argument);
    break;
  case ARCH_GET_CPUID:
  case ARCH_SET_CPUID:
  case ARCH_REQ_XCOMP_PERM:
  case ARCH_REQ_XCOMP_GUEST_PERM:
  case ARCH_ENABLE_TAGGED_ADDR:
  case ARCH_FORCE_TAGGED_SVA:
  case ARCH_SHSTK_ENABLE:
  case ARCH_SHSTK_DISABLE:
  case ARCH_SHSTK_LOCK:
  case ARCH_SHSTK_UNLOCK:
    break;
  default:
    UNREACHABLE("unsupported arch_prctl operation");
  }

  const sptr result =
      executor(SYS_arch_prctl, (sptr)operation, (sptr)argument, 0, 0, 0, 0);
  return result;
}
#endif

static sptr SYSCALL_HANDLER(futex)(const SyscallExecutor &executor,
                                   int *address,
                                   int operation, int value,
                                   sptr timeout_or_value, int *second_address,
                                   int third_value) {
  address = checkAndStrip(address, sizeof(int));

  const int command = operation & FUTEX_CMD_MASK;
  switch (command) {
  case FUTEX_WAIT:
  case FUTEX_LOCK_PI:
  case FUTEX_WAIT_BITSET:
  case FUTEX_LOCK_PI2: {
    const struct timespec *timeout = (const struct timespec *)timeout_or_value;
    timeout =
        checkAndStrip(timeout, sizeof(struct timespec), /*optional=*/true);
    timeout_or_value = (sptr)timeout;
    break;
  }
  case FUTEX_WAIT_REQUEUE_PI: {
    const struct timespec *timeout = (const struct timespec *)timeout_or_value;
    timeout =
        checkAndStrip(timeout, sizeof(struct timespec), /*optional=*/true);
    timeout_or_value = (sptr)timeout;
    second_address = checkAndStrip(second_address, sizeof(int));
    break;
  }
  case FUTEX_REQUEUE:
  case FUTEX_CMP_REQUEUE:
  case FUTEX_WAKE_OP:
  case FUTEX_CMP_REQUEUE_PI:
    second_address = checkAndStrip(second_address, sizeof(int));
    break;
  case FUTEX_WAKE:
  case FUTEX_FD:
  case FUTEX_UNLOCK_PI:
  case FUTEX_TRYLOCK_PI:
  case FUTEX_WAKE_BITSET:
    break;
  default:
    UNREACHABLE("unsupported futex operation");
  }

  const sptr result =
      executor(SYS_futex, (sptr)address, (sptr)operation, (sptr)value,
               timeout_or_value, (sptr)second_address, (sptr)third_value);
  return result;
}

static sptr SYSCALL_HANDLER(set_tid_address)(const SyscallExecutor &executor,
                                             int *tid_address) {
  // The kernel retains this address and clears it at thread exit. Musl keeps
  // the integer live for that entire interval.
  tid_address = checkAndStrip(tid_address, sizeof(int));
  const sptr result =
      executor(SYS_set_tid_address, (sptr)tid_address, 0, 0, 0, 0, 0);
  return result;
}

// fcntl's command determines whether its third argument is a scalar or points
// to one of a small set of fixed-size public layouts.
static sptr SYSCALL_HANDLER(fcntl)(const SyscallExecutor &executor, int fd,
                                   int command, unsigned long argument) {
  switch (command) {
  case F_GETLK:
  case F_SETLK:
  case F_SETLKW:
  case F_OFD_GETLK:
  case F_OFD_SETLK:
  case F_OFD_SETLKW: {
    struct flock *lock = (struct flock *)argument;
    lock = checkAndStrip(lock, sizeof(struct flock));
    argument = (unsigned long)lock;
    break;
  }
  case F_GETOWN_EX:
  case F_SETOWN_EX: {
    struct f_owner_ex *owner = (struct f_owner_ex *)argument;
    owner = checkAndStrip(owner, sizeof(struct f_owner_ex));
    argument = (unsigned long)owner;
    break;
  }
  case F_GETOWNER_UIDS: {
    uid_t *owners = (uid_t *)argument;
    owners = checkAndStrip(owners, 2 * sizeof(uid_t));
    argument = (unsigned long)owners;
    break;
  }
  case F_GET_RW_HINT:
  case F_SET_RW_HINT:
  case F_GET_FILE_RW_HINT:
  case F_SET_FILE_RW_HINT: {
    uint64_t *hint = (uint64_t *)argument;
    hint = checkAndStrip(hint, sizeof(uint64_t));
    argument = (unsigned long)hint;
    break;
  }
  case F_DUPFD:
  case F_GETFD:
  case F_SETFD:
  case F_GETFL:
  case F_SETFL:
  case F_SETOWN:
  case F_GETOWN:
  case F_SETSIG:
  case F_GETSIG:
  case F_SETLEASE:
  case F_GETLEASE:
  case F_NOTIFY:
  case F_CANCELLK:
  case F_DUPFD_CLOEXEC:
  case F_SETPIPE_SZ:
  case F_GETPIPE_SZ:
  case F_ADD_SEALS:
  case F_GET_SEALS:
    break;
  default:
    UNREACHABLE("unsupported fcntl command");
  }

  const sptr result =
      executor(SYS_fcntl, (sptr)fd, (sptr)command, (sptr)argument, 0, 0, 0);
  return result;
}

// ioctl request numbers select the type of the variadic third argument. Keep
// this as an explicit list: many legacy requests do not encode their layout,
// while some device-specific structures contain nested pointers.
static sptr SYSCALL_HANDLER(ioctl)(const SyscallExecutor &executor, int fd,
                                   unsigned request, unsigned long argument) {
  switch (request) {
  case TCGETS:
  case TCSETS:
  case TCSETSW:
  case TCSETSF: {
    struct termios *settings = (struct termios *)argument;
    settings = checkAndStrip(settings, sizeof(struct termios));
    argument = (unsigned long)settings;
    break;
  }
  case TIOCGWINSZ:
  case TIOCSWINSZ: {
    struct winsize *window = (struct winsize *)argument;
    window = checkAndStrip(window, sizeof(struct winsize));
    argument = (unsigned long)window;
    break;
  }
  case FIONREAD:
  case FIONBIO:
  case TIOCGPGRP:
  case TIOCSPGRP:
  case TIOCSPTLCK:
  case SIOCATMARK: {
    int *value = (int *)argument;
    value = checkAndStrip(value, sizeof(int));
    argument = (unsigned long)value;
    break;
  }
  case TIOCGPTN: {
    unsigned *number = (unsigned *)argument;
    number = checkAndStrip(number, sizeof(unsigned));
    argument = (unsigned long)number;
    break;
  }
  case SIOCGIFNAME:
  case SIOCGIFINDEX: {
    struct ifreq *interface = (struct ifreq *)argument;
    interface = checkAndStrip(interface, sizeof(struct ifreq));
    argument = (unsigned long)interface;
    break;
  }
  case TCSBRK:
  case TCXONC:
  case TCFLSH:
  case TIOCSCTTY:
  case FIOCLEX:
  case FIONCLEX:
    break;
  default:
    Report("PTSan does not support ioctl request 0x%x\n", request);
    internal__exit(1);
  }

  const sptr result =
      executor(SYS_ioctl, (sptr)fd, (sptr)request, (sptr)argument, 0, 0, 0);
  return result;
}

// <sys/klog.h> exposes only klogctl(), so name Linux's finite action values
// here. Only the three read actions access the buffer.
enum class SyslogAction {
  Close = 0,
  Open = 1,
  Read = 2,
  ReadAll = 3,
  ReadClear = 4,
  Clear = 5,
  ConsoleOff = 6,
  ConsoleOn = 7,
  ConsoleLevel = 8,
  SizeUnread = 9,
  SizeBuffer = 10,
};

static sptr SYSCALL_HANDLER(syslog)(const SyscallExecutor &executor, int action,
                                    char *buffer, int length) {
  switch ((SyslogAction)action) {
  case SyslogAction::Read:
  case SyslogAction::ReadAll:
  case SyslogAction::ReadClear: {
    uptr buffer_size = 0;
    if (length > 0) {
      buffer_size = (uptr)length;
    }
    buffer = checkAndStrip(buffer, buffer_size);
    break;
  }
  case SyslogAction::Close:
  case SyslogAction::Open:
  case SyslogAction::Clear:
  case SyslogAction::ConsoleOff:
  case SyslogAction::ConsoleOn:
  case SyslogAction::ConsoleLevel:
  case SyslogAction::SizeUnread:
  case SyslogAction::SizeBuffer:
    buffer = clear_id(buffer);
    break;
  default:
    UNREACHABLE("unsupported syslog action");
  }

  const sptr result =
      executor(SYS_syslog, (sptr)action, (sptr)buffer, (sptr)length, 0, 0, 0);
  return result;
}

// The command selects which fixed-size public structure the kernel accesses,
// or whether it ignores the buffer. Musl adds Linux's IPC_64 flag before the
// syscall, so ignore that flag when selecting the structure.
static constexpr int LinuxIpc64Flag = 0x100;

static sptr SYSCALL_HANDLER(msgctl)(const SyscallExecutor &executor,
                                    int queue_id,
                                    int command, struct msqid_ds *buffer) {
  const int operation = command & ~LinuxIpc64Flag;
  switch (operation) {
  case IPC_SET:
  case IPC_STAT:
  case MSG_STAT:
  case MSG_STAT_ANY:
    buffer = checkAndStrip(buffer, sizeof(struct msqid_ds));
    break;
  case IPC_INFO:
  case MSG_INFO: {
    struct msginfo *info = (struct msginfo *)buffer;
    info = checkAndStrip(info, sizeof(struct msginfo));
    buffer = (struct msqid_ds *)info;
    break;
  }
  case IPC_RMID:
    buffer = clear_id(buffer);
    break;
  default:
    UNREACHABLE("unsupported msgctl operation");
  }

  const sptr result = executor(SYS_msgctl, (sptr)queue_id, (sptr)command,
                               (sptr)buffer, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(shmctl)(const SyscallExecutor &executor,
                                    int shared_memory_id, int command,
                                    struct shmid_ds *buffer) {
  const int operation = command & ~LinuxIpc64Flag;
  switch (operation) {
  case IPC_SET:
  case IPC_STAT:
  case SHM_STAT:
  case SHM_STAT_ANY:
    buffer = checkAndStrip(buffer, sizeof(struct shmid_ds));
    break;
  case IPC_INFO: {
    struct shminfo *info = (struct shminfo *)buffer;
    info = checkAndStrip(info, sizeof(struct shminfo));
    buffer = (struct shmid_ds *)info;
    break;
  }
  case SHM_INFO: {
    struct shm_info *info = (struct shm_info *)buffer;
    info = checkAndStrip(info, sizeof(struct shm_info));
    buffer = (struct shmid_ds *)info;
    break;
  }
  case IPC_RMID:
  case SHM_LOCK:
  case SHM_UNLOCK:
    buffer = clear_id(buffer);
    break;
  default:
    UNREACHABLE("unsupported shmctl operation");
  }

  const sptr result = executor(SYS_shmctl, (sptr)shared_memory_id,
                               (sptr)command, (sptr)buffer, 0, 0, 0);
  return result;
}

//===----------------------------------------------------------------------===//
// Nested pointer graphs
//===----------------------------------------------------------------------===//

static uptr saturatingMultiply(uptr left, uptr right) {
  uptr result;
  if (__builtin_mul_overflow(left, right, &result)) {
    return ~uptr(0);
  }
  return result;
}

static uptr saturatingAdd(uptr left, uptr right) {
  uptr result;
  if (__builtin_add_overflow(left, right, &result)) {
    return ~uptr(0);
  }
  return result;
}

// Exec argument vectors are NULL-terminated arrays of pointers to separately
// bounded C strings. Build kernel-facing pointer arrays without copying the
// string bytes themselves.
static char **prepareCStringVector(char *const *strings) {
  if (!strings) {
    return nullptr;
  }

  char *const *source = clear_id(strings);
  const uptr valid_slots = bytesRemainingInObject(strings) / sizeof(char *);
  uptr vector_slots = 0;
  // Go through strings and a.) range check and b.) calculate length of alloca
  do {
    // `==` so we don't spam
    if (vector_slots == valid_slots) {
      ptsan_complain(0, 0xdeadbeef, get_id(strings), "libc",
                     get_addr(strings + vector_slots), sizeof(char *));
    }
  } while (source[vector_slots++]);

  // Now actually check individual strings
  char **shadow = (char **)InternalAlloc(vector_slots * sizeof(char *));
  for (uptr index = 0; index != vector_slots - 1; ++index) {
    shadow[index] = (char *)checkAndStripCString(source[index]);
  }
  shadow[vector_slots - 1] = nullptr;
  return shadow;
}

static sptr SYSCALL_HANDLER(execve)(const SyscallExecutor &executor,
                                    const char *path,
                                    char *const *arguments,
                                    char *const *environment) {
  path = checkAndStripCString(path);
  char **prepared_arguments = prepareCStringVector(arguments);
  char **prepared_environment = prepareCStringVector(environment);

  const sptr result = executor(SYS_execve, (sptr)path, (sptr)prepared_arguments,
                               (sptr)prepared_environment, 0, 0, 0);

  // A successful exec never returns. These allocations exist only on failure.
  if (prepared_environment) {
    InternalFree(prepared_environment);
  }
  if (prepared_arguments) {
    InternalFree(prepared_arguments);
  }
  return result;
}

static sptr SYSCALL_HANDLER(execveat)(const SyscallExecutor &executor,
                                      int dirfd,
                                      const char *path, char *const *arguments,
                                      char *const *environment, int flags) {
  path = checkAndStripCString(path);
  char **prepared_arguments = prepareCStringVector(arguments);
  char **prepared_environment = prepareCStringVector(environment);

  const sptr result =
      executor(SYS_execveat, (sptr)dirfd, (sptr)path, (sptr)prepared_arguments,
               (sptr)prepared_environment, (sptr)flags, 0);

  // A successful exec never returns. These allocations exist only on failure.
  if (prepared_environment) {
    InternalFree(prepared_environment);
  }
  if (prepared_arguments) {
    InternalFree(prepared_arguments);
  }
  return result;
}

// Epoll event data is an opaque cookie. Strip only the outer event pointers.

static sptr SYSCALL_HANDLER(epoll_ctl)(const SyscallExecutor &executor,
                                       int epoll_fd,
                                       int operation, int fd,
                                       struct epoll_event *event) {
  if (operation == EPOLL_CTL_ADD || operation == EPOLL_CTL_MOD) {
    checkMemoryRange(event, sizeof(struct epoll_event));
  }
  event = clear_id(event);
  const sptr result = executor(SYS_epoll_ctl, (sptr)epoll_fd, (sptr)operation,
                               (sptr)fd, (sptr)event, 0, 0);
  return result;
}

#ifdef SYS_epoll_wait
static sptr SYSCALL_HANDLER(epoll_wait)(const SyscallExecutor &executor,
                                        int epoll_fd,
                                        struct epoll_event *events,
                                        int max_events, int timeout) {
  if (max_events > 0) {
    const uptr event_bytes = (uptr)max_events * sizeof(struct epoll_event);
    checkMemoryRange(events, event_bytes);
  }
  events = clear_id(events);
  const sptr result = executor(SYS_epoll_wait, (sptr)epoll_fd, (sptr)events,
                               (sptr)max_events, (sptr)timeout, 0, 0);
  return result;
}
#endif

static sptr SYSCALL_HANDLER(epoll_pwait)(const SyscallExecutor &executor,
                                         int epoll_fd,
                                         struct epoll_event *events,
                                         int max_events, int timeout,
                                         const void *signal_mask,
                                         unsigned long signal_mask_size) {
  if (max_events > 0) {
    const uptr event_bytes = (uptr)max_events * sizeof(struct epoll_event);
    checkMemoryRange(events, event_bytes);
  }
  events = clear_id(events);
  signal_mask = checkAndStrip(signal_mask, sizeof(sigset_t),
                              /*optional=*/true);
  const sptr result =
      executor(SYS_epoll_pwait, (sptr)epoll_fd, (sptr)events, (sptr)max_events,
               (sptr)timeout, (sptr)signal_mask, (sptr)signal_mask_size);
  return result;
}

// Bounded descriptor sets and dynamically sized buffers.

static uptr fileDescriptorSetSize(int nfds) {
  if (nfds <= 0) {
    return 0;
  }

  // Linux's select ABI treats each descriptor set as an array of unsigned
  // long words containing nfds bits. This permits compact sets smaller than
  // libc's fixed-size fd_set, as used by language runtimes such as Perl.
  const uptr bits_per_word = sizeof(unsigned long) * 8;
  const uptr word_count = ((uptr)nfds + bits_per_word - 1) / bits_per_word;
  return word_count * sizeof(unsigned long);
}

static sptr SYSCALL_HANDLER(pselect6)(const SyscallExecutor &executor, int nfds,
                                      void *read_fds, void *write_fds,
                                      void *except_fds,
                                      struct timespec *timeout,
                                      const sptr *signal_mask_descriptor) {
  const uptr set_size = fileDescriptorSetSize(nfds);
  read_fds = checkAndStrip(read_fds, set_size, /*optional=*/true);
  write_fds = checkAndStrip(write_fds, set_size, /*optional=*/true);
  except_fds = checkAndStrip(except_fds, set_size, /*optional=*/true);
  timeout = checkAndStrip(timeout, struct_timespec_sz, /*optional=*/true);

  sptr prepared_signal_mask[2];
  const sptr *prepared_descriptor = nullptr;
  if (signal_mask_descriptor) {
    const sptr *source =
        checkAndStrip(signal_mask_descriptor, 2 * sizeof(sptr));
    const void *signal_mask = (const void *)source[0];
    const uptr signal_mask_size = (uptr)source[1];
    signal_mask =
        checkAndStrip(signal_mask, sizeof(sigset_t), /*optional=*/true);
    prepared_signal_mask[0] = (sptr)signal_mask;
    prepared_signal_mask[1] = (sptr)signal_mask_size;
    prepared_descriptor = prepared_signal_mask;
  }

  const sptr result =
      executor(SYS_pselect6, (sptr)nfds, (sptr)read_fds, (sptr)write_fds,
               (sptr)except_fds, (sptr)timeout, (sptr)prepared_descriptor);
  return result;
}

static sptr SYSCALL_HANDLER(mincore)(const SyscallExecutor &executor,
                                     void *address,
                                     uptr length, unsigned char *vector) {
  const uptr page_size = GetPageSizeCached();
  const uptr vector_size = length / page_size + (length % page_size != 0);
  // This is a virtual-memory range, not an ordinary PTSan object. It may
  // legally cross object boundaries; only the separately bounded output
  // vector receives an object-range check.
  address = clear_id(address);
  vector = checkAndStrip(vector, vector_size);
  const sptr result =
      executor(SYS_mincore, (sptr)address, (sptr)length, (sptr)vector, 0, 0, 0);
  return result;
}

static __sanitizer_file_handle *
checkAndStripFileHandle(__sanitizer_file_handle *handle) {
  const uptr header_size =
      __builtin_offsetof(__sanitizer_file_handle, f_handle);
  __sanitizer_file_handle *source = clear_id(handle);
  if (!checkMemoryRange(handle, header_size)) {
    return source;
  }

  // Linux rejects larger handles after reading only the fixed header.
  if (source->handle_bytes <= 128) {
    checkMemoryRange(handle, header_size + source->handle_bytes);
  }
  return source;
}

static sptr SYSCALL_HANDLER(name_to_handle_at)(const SyscallExecutor &executor,
                                               int dirfd, const char *path,
                                               __sanitizer_file_handle *handle,
                                               int *mount_id, int flags) {
  path = checkAndStripCString(path);
  handle = checkAndStripFileHandle(handle);
  mount_id = checkAndStrip(mount_id, sizeof(int));
  const sptr result = executor(SYS_name_to_handle_at, (sptr)dirfd, (sptr)path,
                               (sptr)handle, (sptr)mount_id, (sptr)flags, 0);
  return result;
}

static sptr SYSCALL_HANDLER(open_by_handle_at)(const SyscallExecutor &executor,
                                               int mount_fd,
                                               __sanitizer_file_handle *handle,
                                               int flags) {
  handle = checkAndStripFileHandle(handle);
  const sptr result = executor(SYS_open_by_handle_at, (sptr)mount_fd,
                               (sptr)handle, (sptr)flags, 0, 0, 0);
  return result;
}

// Signal and timer syscalls preserve opaque signal cookies while stripping
// only the outer pointers passed to the kernel.

static sptr
SYSCALL_HANDLER(rt_sigaction)(const SyscallExecutor &executor, int signal,
                              const __sanitizer_kernel_sigaction_t *action,
                              __sanitizer_kernel_sigaction_t *old_action,
                              unsigned long signal_mask_size) {
  action = checkAndStrip(action, sizeof(__sanitizer_kernel_sigaction_t),
                         /*optional=*/true);
  old_action = checkAndStrip(old_action, sizeof(__sanitizer_kernel_sigaction_t),
                             /*optional=*/true);
  const sptr result = executor(SYS_rt_sigaction, (sptr)signal, (sptr)action,
                               (sptr)old_action, (sptr)signal_mask_size, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(rt_sigqueueinfo)(const SyscallExecutor &executor,
                                             pid_t process_id, int signal,
                                             const siginfo_t *info) {
  info = checkAndStrip(info, sizeof(siginfo_t));
  const sptr result = executor(SYS_rt_sigqueueinfo, (sptr)process_id,
                               (sptr)signal, (sptr)info, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(rt_sigtimedwait)(const SyscallExecutor &executor,
                                             const sigset_t *signal_set,
                                             siginfo_t *info,
                                             const struct timespec *timeout,
                                             unsigned long signal_set_size) {
  signal_set = checkAndStrip(signal_set, sizeof(sigset_t));
  info = checkAndStrip(info, sizeof(siginfo_t), /*optional=*/true);
  timeout = checkAndStrip(timeout, sizeof(struct timespec), /*optional=*/true);
  const sptr result =
      executor(SYS_rt_sigtimedwait, (sptr)signal_set, (sptr)info, (sptr)timeout,
               (sptr)signal_set_size, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(timer_create)(const SyscallExecutor &executor,
                                          clockid_t clock_id, const void *event,
                                          int *timer_id) {
  // Linux copies a public sigevent, but musl intentionally supplies only this
  // compact prefix because the remaining bytes are ignored for its supported
  // modes. Treat sigval as an opaque cookie rather than a nested pointer.
  const uptr musl_event_size =
      RoundUpTo(sizeof(union sigval) + 3 * sizeof(int), alignof(union sigval));
  event = checkAndStrip(event, musl_event_size, /*optional=*/true);
  timer_id = checkAndStrip(timer_id, sizeof(int));
  const sptr result = executor(SYS_timer_create, (sptr)clock_id, (sptr)event,
                               (sptr)timer_id, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(mq_notify)(const SyscallExecutor &executor,
                                       mqd_t descriptor,
                                       const struct sigevent *event) {
  if (!event) {
    return executor(SYS_mq_notify, (sptr)descriptor, 0, 0, 0, 0, 0);
  }

  const bool event_is_valid = checkMemoryRange(event, sizeof(struct sigevent));
  const struct sigevent *source = clear_id(event);
  if (source->sigev_notify != SIGEV_THREAD) {
    // For signal delivery, sigev_value is an opaque value returned to the
    // process. Preserve it even when its pointer member contains an ID.
    return executor(SYS_mq_notify, (sptr)descriptor, (sptr)source, 0, 0, 0, 0);
  }

  // Linux interprets sigev_value.sival_ptr as a pointer to a 32-byte netlink
  // cookie for SIGEV_THREAD. Musl uses this form internally to implement the
  // public thread-notification API.
  static constexpr uptr MqNotifyCookieSize = 32;
  if (event_is_valid) {
    checkMemoryRange(source->sigev_value.sival_ptr, MqNotifyCookieSize);
  }
  struct sigevent shadow = *source;
  shadow.sigev_value.sival_ptr = clear_id(source->sigev_value.sival_ptr);
  return executor(SYS_mq_notify, (sptr)descriptor, (sptr)&shadow, 0, 0, 0, 0);
}

// Vectored and message I/O use counted arrays and explicit payload lengths.

static bool shouldCheckIovecs(uptr vector_count) {
  return vector_count != 0 && vector_count <= UIO_MAXIOV;
}

static __sanitizer_iovec *prepareIovecs(const __sanitizer_iovec *vectors,
                                        uptr vector_count,
                                        bool check_vector_contents = true) {
  const __sanitizer_iovec *source = clear_id(vectors);
  const uptr vector_bytes = vector_count * sizeof(__sanitizer_iovec);
  checkMemoryRange(vectors, vector_bytes);
  // The remote iovec payloads in process_vm_* are addresses in another
  // process. Only payloads in this process can be checked against our bounds
  // metadata; the iovec descriptor array itself is always checked.
  if (check_vector_contents) {
    for (uptr index = 0; index != vector_count; ++index) {
      checkMemoryRange(source[index].iov_base, source[index].iov_len);
    }
  }

  __sanitizer_iovec *shadow = (__sanitizer_iovec *)InternalAlloc(vector_bytes);
  for (uptr index = 0; index != vector_count; ++index) {
    shadow[index].iov_base = clear_id(source[index].iov_base);
    shadow[index].iov_len = source[index].iov_len;
  }
  return shadow;
}

static bool prepareMessageContents(__sanitizer_msghdr *message) {
  message->msg_name = checkAndStrip(message->msg_name, message->msg_namelen);

  const bool allocated_iovecs = shouldCheckIovecs(message->msg_iovlen);
  if (allocated_iovecs) {
    message->msg_iov = prepareIovecs(message->msg_iov, message->msg_iovlen);
  } else {
    message->msg_iov = clear_id(message->msg_iov);
  }

  message->msg_control =
      checkAndStrip(message->msg_control, message->msg_controllen);
  return allocated_iovecs;
}

static sptr SYSCALL_HANDLER(readv)(const SyscallExecutor &executor, int fd,
                                   const __sanitizer_iovec *vectors,
                                   int vector_count) {
  if (!shouldCheckIovecs((uptr)vector_count)) {
    return executor(SYS_readv, (sptr)fd, (sptr)clear_id(vectors),
                    (sptr)vector_count, 0, 0, 0);
  }
  __sanitizer_iovec *prepared = prepareIovecs(vectors, (uptr)vector_count);
  const sptr result = executor(SYS_readv, (sptr)fd, (sptr)prepared,
                               (sptr)vector_count, 0, 0, 0);
  InternalFree(prepared);
  return result;
}

static sptr SYSCALL_HANDLER(writev)(const SyscallExecutor &executor, int fd,
                                    const __sanitizer_iovec *vectors,
                                    int vector_count) {
  if (!shouldCheckIovecs((uptr)vector_count)) {
    return executor(SYS_writev, (sptr)fd, (sptr)clear_id(vectors),
                    (sptr)vector_count, 0, 0, 0);
  }
  __sanitizer_iovec *prepared = prepareIovecs(vectors, (uptr)vector_count);
  const sptr result = executor(SYS_writev, (sptr)fd, (sptr)prepared,
                               (sptr)vector_count, 0, 0, 0);
  InternalFree(prepared);
  return result;
}

static sptr SYSCALL_HANDLER(preadv)(const SyscallExecutor &executor, int fd,
                                    const __sanitizer_iovec *vectors,
                                    int vector_count, long offset_low,
                                    long offset_high) {
  if (!shouldCheckIovecs((uptr)vector_count)) {
    return executor(SYS_preadv, (sptr)fd, (sptr)clear_id(vectors),
                    (sptr)vector_count, (sptr)offset_low, (sptr)offset_high, 0);
  }
  __sanitizer_iovec *prepared = prepareIovecs(vectors, (uptr)vector_count);
  const sptr result =
      executor(SYS_preadv, (sptr)fd, (sptr)prepared, (sptr)vector_count,
               (sptr)offset_low, (sptr)offset_high, 0);
  InternalFree(prepared);
  return result;
}

static sptr SYSCALL_HANDLER(pwritev)(const SyscallExecutor &executor, int fd,
                                     const __sanitizer_iovec *vectors,
                                     int vector_count, long offset_low,
                                     long offset_high) {
  if (!shouldCheckIovecs((uptr)vector_count)) {
    return executor(SYS_pwritev, (sptr)fd, (sptr)clear_id(vectors),
                    (sptr)vector_count, (sptr)offset_low, (sptr)offset_high, 0);
  }
  __sanitizer_iovec *prepared = prepareIovecs(vectors, (uptr)vector_count);
  const sptr result =
      executor(SYS_pwritev, (sptr)fd, (sptr)prepared, (sptr)vector_count,
               (sptr)offset_low, (sptr)offset_high, 0);
  InternalFree(prepared);
  return result;
}

static sptr SYSCALL_HANDLER(preadv2)(const SyscallExecutor &executor, int fd,
                                     const __sanitizer_iovec *vectors,
                                     int vector_count, long offset_low,
                                     long offset_high, int flags) {
  if (!shouldCheckIovecs((uptr)vector_count)) {
    return executor(SYS_preadv2, (sptr)fd, (sptr)clear_id(vectors),
                    (sptr)vector_count, (sptr)offset_low, (sptr)offset_high,
                    (sptr)flags);
  }
  __sanitizer_iovec *prepared = prepareIovecs(vectors, (uptr)vector_count);
  const sptr result =
      executor(SYS_preadv2, (sptr)fd, (sptr)prepared, (sptr)vector_count,
               (sptr)offset_low, (sptr)offset_high, (sptr)flags);
  InternalFree(prepared);
  return result;
}

static sptr SYSCALL_HANDLER(pwritev2)(const SyscallExecutor &executor, int fd,
                                      const __sanitizer_iovec *vectors,
                                      int vector_count, long offset_low,
                                      long offset_high, int flags) {
  if (!shouldCheckIovecs((uptr)vector_count)) {
    return executor(SYS_pwritev2, (sptr)fd, (sptr)clear_id(vectors),
                    (sptr)vector_count, (sptr)offset_low, (sptr)offset_high,
                    (sptr)flags);
  }
  __sanitizer_iovec *prepared = prepareIovecs(vectors, (uptr)vector_count);
  const sptr result =
      executor(SYS_pwritev2, (sptr)fd, (sptr)prepared, (sptr)vector_count,
               (sptr)offset_low, (sptr)offset_high, (sptr)flags);
  InternalFree(prepared);
  return result;
}

static sptr SYSCALL_HANDLER(vmsplice)(const SyscallExecutor &executor, int fd,
                                      const __sanitizer_iovec *vectors,
                                      uptr vector_count, unsigned flags) {
  if (!shouldCheckIovecs(vector_count)) {
    return executor(SYS_vmsplice, (sptr)fd, (sptr)clear_id(vectors),
                    (sptr)vector_count, (sptr)flags, 0, 0);
  }
  __sanitizer_iovec *prepared = prepareIovecs(vectors, vector_count);
  const sptr result = executor(SYS_vmsplice, (sptr)fd, (sptr)prepared,
                               (sptr)vector_count, (sptr)flags, 0, 0);
  InternalFree(prepared);
  return result;
}

static sptr SYSCALL_HANDLER(process_vm_readv)(
    const SyscallExecutor &executor, int pid, const __sanitizer_iovec *local_vectors,
    uptr local_vector_count, const __sanitizer_iovec *remote_vectors,
    uptr remote_vector_count, uptr flags) {
  if (flags || !shouldCheckIovecs(local_vector_count) ||
      !shouldCheckIovecs(remote_vector_count)) {
    return executor(SYS_process_vm_readv, (sptr)pid,
                    (sptr)clear_id(local_vectors), (sptr)local_vector_count,
                    (sptr)clear_id(remote_vectors), (sptr)remote_vector_count,
                    (sptr)flags);
  }
  __sanitizer_iovec *local = prepareIovecs(local_vectors, local_vector_count);
  __sanitizer_iovec *remote =
      prepareIovecs(remote_vectors, remote_vector_count, false);
  const sptr result = executor(SYS_process_vm_readv, (sptr)pid, (sptr)local,
                               (sptr)local_vector_count, (sptr)remote,
                               (sptr)remote_vector_count, (sptr)flags);
  InternalFree(remote);
  InternalFree(local);
  return result;
}

static sptr SYSCALL_HANDLER(process_vm_writev)(
    const SyscallExecutor &executor, int pid, const __sanitizer_iovec *local_vectors,
    uptr local_vector_count, const __sanitizer_iovec *remote_vectors,
    uptr remote_vector_count, uptr flags) {
  if (flags || !shouldCheckIovecs(local_vector_count) ||
      !shouldCheckIovecs(remote_vector_count)) {
    return executor(SYS_process_vm_writev, (sptr)pid,
                    (sptr)clear_id(local_vectors), (sptr)local_vector_count,
                    (sptr)clear_id(remote_vectors), (sptr)remote_vector_count,
                    (sptr)flags);
  }
  __sanitizer_iovec *local = prepareIovecs(local_vectors, local_vector_count);
  __sanitizer_iovec *remote =
      prepareIovecs(remote_vectors, remote_vector_count, false);
  const sptr result = executor(SYS_process_vm_writev, (sptr)pid, (sptr)local,
                               (sptr)local_vector_count, (sptr)remote,
                               (sptr)remote_vector_count, (sptr)flags);
  InternalFree(remote);
  InternalFree(local);
  return result;
}

static sptr SYSCALL_HANDLER(sendmsg)(const SyscallExecutor &executor,
                                     int socket_fd,
                                     const __sanitizer_msghdr *message,
                                     int flags) {
  const __sanitizer_msghdr *source =
      checkAndStrip(message, sizeof(__sanitizer_msghdr));
  __sanitizer_msghdr shadow = *source;
  const bool allocated_iovecs = prepareMessageContents(&shadow);
  const sptr result = executor(SYS_sendmsg, (sptr)socket_fd, (sptr)&shadow,
                               (sptr)flags, 0, 0, 0);
  if (allocated_iovecs) {
    InternalFree(shadow.msg_iov);
  }
  return result;
}

static sptr SYSCALL_HANDLER(recvmsg)(const SyscallExecutor &executor,
                                     int socket_fd,
                                     __sanitizer_msghdr *message, int flags) {
  __sanitizer_msghdr *destination =
      checkAndStrip(message, sizeof(__sanitizer_msghdr));
  __sanitizer_msghdr shadow = *destination;
  const bool allocated_iovecs = prepareMessageContents(&shadow);
  const sptr result = executor(SYS_recvmsg, (sptr)socket_fd, (sptr)&shadow,
                               (sptr)flags, 0, 0, 0);
  destination->msg_namelen = shadow.msg_namelen;
  destination->msg_controllen = shadow.msg_controllen;
  destination->msg_flags = shadow.msg_flags;
  if (allocated_iovecs) {
    InternalFree(shadow.msg_iov);
  }
  return result;
}

static sptr SYSCALL_HANDLER(recvmmsg)(const SyscallExecutor &executor,
                                      int socket_fd,
                                      __sanitizer_mmsghdr *messages,
                                      uptr message_count, unsigned flags,
                                      struct timespec *timeout) {
  __sanitizer_mmsghdr *destination = clear_id(messages);
  struct timespec *kernel_timeout = clear_id(timeout);
  uptr kernel_count = message_count;
  if (kernel_count > UIO_MAXIOV) {
    kernel_count = UIO_MAXIOV;
  }
  if (kernel_count == 0) {
    return executor(SYS_recvmmsg, (sptr)socket_fd, (sptr)destination,
                    (sptr)message_count, (sptr)flags, (sptr)kernel_timeout, 0);
  }

  kernel_timeout = checkAndStrip(timeout, struct_timespec_sz,
                                 /*optional=*/true);
  const uptr message_bytes = kernel_count * sizeof(__sanitizer_mmsghdr);
  destination = checkAndStrip(messages, message_bytes);

  __sanitizer_mmsghdr *shadow =
      (__sanitizer_mmsghdr *)InternalAlloc(message_bytes);
  __sanitizer_iovec **allocations = (__sanitizer_iovec **)InternalAlloc(
      kernel_count * sizeof(__sanitizer_iovec *));
  uptr allocation_count = 0;
  for (uptr index = 0; index != kernel_count; ++index) {
    shadow[index] = destination[index];
    if (prepareMessageContents(&shadow[index].msg_hdr)) {
      allocations[allocation_count++] = shadow[index].msg_hdr.msg_iov;
    }
  }

  const sptr result =
      executor(SYS_recvmmsg, (sptr)socket_fd, (sptr)shadow, (sptr)message_count,
               (sptr)flags, (sptr)kernel_timeout, 0);
  if (result > 0) {
    uptr completed = (uptr)result;
    if (completed > kernel_count) {
      completed = kernel_count;
    }
    for (uptr index = 0; index != completed; ++index) {
      destination[index].msg_hdr.msg_namelen =
          shadow[index].msg_hdr.msg_namelen;
      destination[index].msg_hdr.msg_controllen =
          shadow[index].msg_hdr.msg_controllen;
      destination[index].msg_hdr.msg_flags = shadow[index].msg_hdr.msg_flags;
      destination[index].msg_len = shadow[index].msg_len;
    }
  }

  for (uptr index = 0; index != allocation_count; ++index) {
    InternalFree(allocations[index]);
  }
  InternalFree(allocations);
  InternalFree(shadow);
  return result;
}

//===----------------------------------------------------------------------===//
// Cancellable, IPC, descriptor-set, and socket handlers
//===----------------------------------------------------------------------===//

static uptr readSocketLengthForValidation(socklen_t *length) {
  if (!length) {
    return 0;
  }
  if (!checkMemoryRange(length, sizeof(socklen_t))) {
    return 0;
  }
  return *clear_id(length);
}

static void requireSupportedFlatSocketOption(int level, int option) {
  switch (level) {
  case SOL_SOCKET:
    switch (option) {
    case SO_REUSEADDR:
    case SO_REUSEPORT:
    case SO_KEEPALIVE:
    case SO_BROADCAST:
    case SO_SNDBUF:
    case SO_RCVBUF:
    case SO_ERROR:
    case SO_TYPE:
    case SO_ACCEPTCONN:
    case SO_LINGER:
      return;
    }
    break;
  case IPPROTO_TCP:
    if (option == TCP_NODELAY) {
      return;
    }
    break;
  case IPPROTO_IPV6:
    if (option == IPV6_V6ONLY) {
      return;
    }
    break;
  }

  Report("PTSan does not support socket option level %d, option %d\n", level,
         option);
  internal__exit(1);
}

static sptr SYSCALL_HANDLER(setsockopt)(const SyscallExecutor &executor,
                                        int socket_fd,
                                        int level, int option,
                                        const void *value,
                                        socklen_t value_size) {
  requireSupportedFlatSocketOption(level, option);
  value = checkAndStrip(value, value_size);
  const sptr result = executor(SYS_setsockopt, (sptr)socket_fd, (sptr)level,
                               (sptr)option, (sptr)value, (sptr)value_size, 0);
  return result;
}

static sptr SYSCALL_HANDLER(getsockopt)(const SyscallExecutor &executor,
                                        int socket_fd,
                                        int level, int option, void *value,
                                        socklen_t *value_size) {
  requireSupportedFlatSocketOption(level, option);

  const uptr initial_size = readSocketLengthForValidation(value_size);
  value = checkAndStrip(value, initial_size);
  value_size = clear_id(value_size);

  const sptr result = executor(SYS_getsockopt, (sptr)socket_fd, (sptr)level,
                               (sptr)option, (sptr)value, (sptr)value_size, 0);
  return result;
}

static sptr SYSCALL_HANDLER(nanosleep)(const SyscallExecutor &executor,
                                       const struct timespec *request,
                                       struct timespec *remaining) {
  request = checkAndStrip(request, struct_timespec_sz);
  remaining = checkAndStrip(remaining, struct_timespec_sz, /*optional=*/true);
  const sptr result =
      executor(SYS_nanosleep, (sptr)request, (sptr)remaining, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(rt_sigsuspend)(const SyscallExecutor &executor,
                                           const void *signal_mask,
                                           unsigned long signal_mask_size) {
  signal_mask = checkAndStrip(signal_mask, sizeof(sigset_t));
  const sptr result = executor(SYS_rt_sigsuspend, (sptr)signal_mask,
                               (sptr)signal_mask_size, 0, 0, 0, 0);
  return result;
}

#ifdef SYS_open
static sptr SYSCALL_HANDLER(open)(const SyscallExecutor &executor,
                                  const char *path,
                                  int flags, unsigned mode) {
  path = checkAndStripCString(path);
  const sptr result =
      executor(SYS_open, (sptr)path, (sptr)flags, (sptr)mode, 0, 0, 0);
  return result;
}
#endif

#ifdef SYS_poll
static sptr SYSCALL_HANDLER(poll)(const SyscallExecutor &executor,
                                  struct pollfd *fds,
                                  unsigned long count, int timeout) {
  fds = checkAndStrip(fds, saturatingMultiply(count, sizeof(struct pollfd)));
  const sptr result =
      executor(SYS_poll, (sptr)fds, (sptr)count, (sptr)timeout, 0, 0, 0);
  return result;
}
#endif

static sptr SYSCALL_HANDLER(clock_nanosleep)(const SyscallExecutor &executor,
                                             int clock_id, int flags,
                                             const struct timespec *request,
                                             struct timespec *remaining) {
  request = checkAndStrip(request, struct_timespec_sz);
  if (flags & TIMER_ABSTIME) {
    remaining = clear_id(remaining);
  } else {
    remaining = checkAndStrip(remaining, struct_timespec_sz,
                              /*optional=*/true);
  }
  const sptr result = executor(SYS_clock_nanosleep, (sptr)clock_id, (sptr)flags,
                               (sptr)request, (sptr)remaining, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(msgsnd)(const SyscallExecutor &executor,
                                    int queue_id,
                                    const void *message,
                                    unsigned long message_size, int flags) {
  message = checkAndStrip(message, saturatingAdd(message_size, sizeof(long)));
  const sptr result = executor(SYS_msgsnd, (sptr)queue_id, (sptr)message,
                               (sptr)message_size, (sptr)flags, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(openat)(const SyscallExecutor &executor, int dirfd,
                                    const char *path, int flags,
                                    unsigned mode) {
  path = checkAndStripCString(path);
  const sptr result = executor(SYS_openat, (sptr)dirfd, (sptr)path, (sptr)flags,
                               (sptr)mode, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(wait4)(const SyscallExecutor &executor, int pid,
                                   int *status, int options,
                                   struct rusage *usage) {
  status = checkAndStrip(status, sizeof(int), /*optional=*/true);
  usage = checkAndStrip(usage, sizeof(struct rusage), /*optional=*/true);
  const sptr result = executor(SYS_wait4, (sptr)pid, (sptr)status,
                               (sptr)options, (sptr)usage, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(mq_timedreceive)(const SyscallExecutor &executor,
                                             int mqdes, void *message,
                                             unsigned long message_size,
                                             unsigned *priority,
                                             const struct timespec *timeout) {
  message = checkAndStrip(message, message_size);
  priority = checkAndStrip(priority, sizeof(unsigned), /*optional=*/true);
  timeout = checkAndStrip(timeout, struct_timespec_sz, /*optional=*/true);
  const sptr result =
      executor(SYS_mq_timedreceive, (sptr)mqdes, (sptr)message,
               (sptr)message_size, (sptr)priority, (sptr)timeout, 0);
  return result;
}

static sptr SYSCALL_HANDLER(mq_timedsend)(const SyscallExecutor &executor,
                                          int mqdes,
                                          const void *message,
                                          unsigned long message_size,
                                          unsigned priority,
                                          const struct timespec *timeout) {
  message = checkAndStrip(message, message_size);
  timeout = checkAndStrip(timeout, struct_timespec_sz, /*optional=*/true);
  const sptr result =
      executor(SYS_mq_timedsend, (sptr)mqdes, (sptr)message, (sptr)message_size,
               (sptr)priority, (sptr)timeout, 0);
  return result;
}

static sptr SYSCALL_HANDLER(msgrcv)(const SyscallExecutor &executor,
                                    int queue_id,
                                    void *message, unsigned long message_size,
                                    long message_type, int flags) {
  message = checkAndStrip(message, saturatingAdd(message_size, sizeof(long)));
  const sptr result =
      executor(SYS_msgrcv, (sptr)queue_id, (sptr)message, (sptr)message_size,
               (sptr)message_type, (sptr)flags, 0);
  return result;
}

static sptr SYSCALL_HANDLER(ppoll)(const SyscallExecutor &executor,
                                   struct pollfd *fds,
                                   unsigned long count,
                                   struct timespec *timeout,
                                   const void *signal_mask,
                                   unsigned long signal_mask_size) {
  fds = checkAndStrip(fds, saturatingMultiply(count, sizeof(struct pollfd)));
  // Linux writes the remaining timeout through the raw ppoll ABI. musl's
  // public wrapper normally supplies a temporary, but this models the ABI.
  timeout = checkAndStrip(timeout, struct_timespec_sz,
                          /*optional=*/true);
  signal_mask = checkAndStrip(signal_mask, sizeof(sigset_t),
                              /*optional=*/true);
  const sptr result = executor(SYS_ppoll, (sptr)fds, (sptr)count, (sptr)timeout,
                               (sptr)signal_mask, (sptr)signal_mask_size, 0);
  return result;
}

#ifdef SYS_select
static sptr SYSCALL_HANDLER(select)(const SyscallExecutor &executor, int nfds,
                                    void *read_fds, void *write_fds,
                                    void *except_fds, struct timeval *timeout) {
  const uptr set_size = fileDescriptorSetSize(nfds);
  read_fds = checkAndStrip(read_fds, set_size,
                           /*optional=*/true);
  write_fds = checkAndStrip(write_fds, set_size,
                            /*optional=*/true);
  except_fds = checkAndStrip(except_fds, set_size,
                             /*optional=*/true);
  timeout = checkAndStrip(timeout, timeval_sz,
                          /*optional=*/true);
  const sptr result =
      executor(SYS_select, (sptr)nfds, (sptr)read_fds, (sptr)write_fds,
               (sptr)except_fds, (sptr)timeout, 0);
  return result;
}
#endif

static sptr SYSCALL_HANDLER(waitid)(const SyscallExecutor &executor,
                                    int id_type,
                                    int id, void *info, int options,
                                    struct rusage *usage) {
  info = checkAndStrip(info, siginfo_t_sz, /*optional=*/true);
  usage = checkAndStrip(usage, sizeof(struct rusage), /*optional=*/true);
  const sptr result = executor(SYS_waitid, (sptr)id_type, (sptr)id, (sptr)info,
                               (sptr)options, (sptr)usage, 0);
  return result;
}

static sptr SYSCALL_HANDLER(accept)(const SyscallExecutor &executor,
                                    int socket_fd,
                                    struct sockaddr *address,
                                    unsigned *address_length) {
  if (address) {
    uptr addressSize = readSocketLengthForValidation(address_length);
    address = checkAndStrip(address, addressSize);
  }
  address = clear_id(address);
  address_length = clear_id(address_length);
  const sptr result = executor(SYS_accept, (sptr)socket_fd, (sptr)address,
                               (sptr)address_length, (sptr)0, (sptr)0, (sptr)0);
  return result;
}

static sptr SYSCALL_HANDLER(accept4)(const SyscallExecutor &executor,
                                     int socket_fd,
                                     struct sockaddr *address,
                                     unsigned *address_length, int flags) {
  if (address) {
    uptr addressSize = readSocketLengthForValidation(address_length);
    address = checkAndStrip(address, addressSize);
  }
  address = clear_id(address);
  address_length = clear_id(address_length);
  const sptr result =
      executor(SYS_accept4, (sptr)socket_fd, (sptr)address,
               (sptr)address_length, (sptr)flags, (sptr)0, (sptr)0);
  return result;
}

static sptr SYSCALL_HANDLER(connect)(const SyscallExecutor &executor,
                                     int socket_fd,
                                     const struct sockaddr *address,
                                     unsigned address_length) {
  address = checkAndStrip(address, address_length);
  const sptr result = executor(SYS_connect, (sptr)socket_fd, (sptr)address,
                               (sptr)address_length, (sptr)0, (sptr)0, (sptr)0);
  return result;
}

static sptr SYSCALL_HANDLER(recvfrom)(const SyscallExecutor &executor,
                                      int socket_fd,
                                      void *buffer, unsigned long length,
                                      int flags,
                                      struct sockaddr *source_address,
                                      unsigned *address_length) {
  buffer = checkAndStrip(buffer, length);
  if (source_address) {
    uptr addressSize = readSocketLengthForValidation(address_length);
    source_address = checkAndStrip(source_address, addressSize);
  }
  source_address = clear_id(source_address);
  address_length = clear_id(address_length);
  const sptr result =
      executor(SYS_recvfrom, (sptr)socket_fd, (sptr)buffer, (sptr)length,
               (sptr)flags, (sptr)source_address, (sptr)address_length);
  return result;
}

static sptr SYSCALL_HANDLER(sendto)(const SyscallExecutor &executor,
                                    int socket_fd,
                                    const void *buffer, unsigned long length,
                                    int flags,
                                    const struct sockaddr *destination_address,
                                    unsigned address_length) {
  buffer = checkAndStrip(buffer, length);
  destination_address = checkAndStrip(destination_address, address_length);
  const sptr result =
      executor(SYS_sendto, (sptr)socket_fd, (sptr)buffer, (sptr)length,
               (sptr)flags, (sptr)destination_address, (sptr)address_length);
  return result;
}

//===----------------------------------------------------------------------===//
// Audited one-argument flat syscall handlers
//===----------------------------------------------------------------------===//

static sptr SYSCALL_HANDLER(acct)(const SyscallExecutor &executor,
                                  const char *path) {
  path = checkAndStripCString(path, /*optional=*/true);
  const sptr result = executor(SYS_acct, (sptr)path, 0, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(adjtimex)(const SyscallExecutor &executor,
                                      struct timex *time_state) {
  time_state = checkAndStrip(time_state, struct_timex_sz);
  const sptr result = executor(SYS_adjtimex, (sptr)time_state, 0, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(chdir)(const SyscallExecutor &executor,
                                   const char *path) {
  path = checkAndStripCString(path);
  const sptr result = executor(SYS_chdir, (sptr)path, 0, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(chroot)(const SyscallExecutor &executor,
                                    const char *path) {
  path = checkAndStripCString(path);
  const sptr result = executor(SYS_chroot, (sptr)path, 0, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(mq_unlink)(const SyscallExecutor &executor,
                                       const char *name) {
  name = checkAndStripCString(name);
  const sptr result = executor(SYS_mq_unlink, (sptr)name, 0, 0, 0, 0, 0);
  return result;
}

#ifdef SYS_pipe
static sptr SYSCALL_HANDLER(pipe)(const SyscallExecutor &executor,
                                  int *pipe_fds) {
  pipe_fds = checkAndStrip(pipe_fds, 2 * sizeof(int));
  const sptr result = executor(SYS_pipe, (sptr)pipe_fds, 0, 0, 0, 0, 0);
  return result;
}
#endif

#ifdef SYS_rmdir
static sptr SYSCALL_HANDLER(rmdir)(const SyscallExecutor &executor,
                                   const char *path) {
  path = checkAndStripCString(path);
  const sptr result = executor(SYS_rmdir, (sptr)path, 0, 0, 0, 0, 0);
  return result;
}
#endif

static sptr SYSCALL_HANDLER(swapoff)(const SyscallExecutor &executor,
                                     const char *path) {
  path = checkAndStripCString(path);
  const sptr result = executor(SYS_swapoff, (sptr)path, 0, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(sysinfo)(const SyscallExecutor &executor,
                                     struct sysinfo *info) {
  info = checkAndStrip(info, sizeof(struct sysinfo));
  const sptr result = executor(SYS_sysinfo, (sptr)info, 0, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(times)(const SyscallExecutor &executor,
                                   struct tms *process_times) {
  process_times =
      checkAndStrip(process_times, struct_tms_sz, /*optional=*/true);
  const sptr result = executor(SYS_times, (sptr)process_times, 0, 0, 0, 0, 0);
  return result;
}

#ifdef SYS_unlink
static sptr SYSCALL_HANDLER(unlink)(const SyscallExecutor &executor,
                                    const char *path) {
  path = checkAndStripCString(path);
  const sptr result = executor(SYS_unlink, (sptr)path, 0, 0, 0, 0, 0);
  return result;
}
#endif

//===----------------------------------------------------------------------===//
// Audited two-argument flat syscall handlers
//===----------------------------------------------------------------------===//

#ifdef SYS_access
static sptr SYSCALL_HANDLER(access)(const SyscallExecutor &executor,
                                    const char *path,
                                    int mode) {
  path = checkAndStripCString(path);
  const sptr result = executor(SYS_access, (sptr)path, (sptr)mode, 0, 0, 0, 0);
  return result;
}
#endif

static uptr capabilityDataSizeForValidation(const void *header) {
  if (!header) {
    return 0;
  }
  if (!checkMemoryRange(header, __user_cap_header_struct_sz)) {
    return 0;
  }
  return __user_cap_data_struct_sz((void *)clear_id(header));
}

static sptr SYSCALL_HANDLER(capget)(const SyscallExecutor &executor,
                                    void *header,
                                    void *data) {
  uptr data_size = capabilityDataSizeForValidation(header);
  header = clear_id(header);
  data = checkAndStrip(data, data_size, /*optional=*/true);
  const sptr result =
      executor(SYS_capget, (sptr)header, (sptr)data, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(capset)(const SyscallExecutor &executor,
                                    const void *header, const void *data) {
  uptr data_size = capabilityDataSizeForValidation(header);
  header = clear_id(header);
  data = checkAndStrip(data, data_size);
  const sptr result =
      executor(SYS_capset, (sptr)header, (sptr)data, 0, 0, 0, 0);
  return result;
}

#ifdef SYS_chmod
static sptr SYSCALL_HANDLER(chmod)(const SyscallExecutor &executor,
                                   const char *path,
                                   unsigned mode) {
  path = checkAndStripCString(path);
  const sptr result = executor(SYS_chmod, (sptr)path, (sptr)mode, 0, 0, 0, 0);
  return result;
}
#endif

static sptr SYSCALL_HANDLER(clock_adjtime)(const SyscallExecutor &executor,
                                           int clock_id,
                                           struct timex *time_state) {
  time_state = checkAndStrip(time_state, struct_timex_sz);
  const sptr result =
      executor(SYS_clock_adjtime, (sptr)clock_id, (sptr)time_state, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(clock_getres)(const SyscallExecutor &executor,
                                          int clock_id,
                                          struct timespec *resolution) {
  resolution = checkAndStrip(resolution, struct_timespec_sz, /*optional=*/true);
  const sptr result =
      executor(SYS_clock_getres, (sptr)clock_id, (sptr)resolution, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(clock_gettime)(const SyscallExecutor &executor,
                                           int clock_id,
                                           struct timespec *time) {
  time = checkAndStrip(time, struct_timespec_sz);
  const sptr result =
      executor(SYS_clock_gettime, (sptr)clock_id, (sptr)time, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(clock_settime)(const SyscallExecutor &executor,
                                           int clock_id,
                                           const struct timespec *time) {
  time = checkAndStrip(time, struct_timespec_sz);
  const sptr result =
      executor(SYS_clock_settime, (sptr)clock_id, (sptr)time, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(delete_module)(const SyscallExecutor &executor,
                                           const char *name, int flags) {
  name = checkAndStripCString(name);
  const sptr result =
      executor(SYS_delete_module, (sptr)name, (sptr)flags, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(fremovexattr)(const SyscallExecutor &executor,
                                          int fd,
                                          const char *name) {
  name = checkAndStripCString(name);
  const sptr result =
      executor(SYS_fremovexattr, (sptr)fd, (sptr)name, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(fstat)(const SyscallExecutor &executor, int fd,
                                   struct stat *status) {
  status = checkAndStrip(status, struct_kernel_stat_sz);
  const sptr result = executor(SYS_fstat, (sptr)fd, (sptr)status, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(fstatfs)(const SyscallExecutor &executor, int fd,
                                     struct statfs *status) {
  status = checkAndStrip(status, struct_statfs_sz);
  const sptr result = executor(SYS_fstatfs, (sptr)fd, (sptr)status, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(getcwd)(const SyscallExecutor &executor,
                                    char *buffer,
                                    unsigned long size) {
  buffer = checkAndStrip(buffer, size);
  const sptr result =
      executor(SYS_getcwd, (sptr)buffer, (sptr)size, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(getgroups)(const SyscallExecutor &executor,
                                       int count,
                                       gid_t *groups) {
  uptr groups_size = 0;
  if (count > 0) {
    groups_size = (uptr)count * sizeof(gid_t);
  }
  groups = checkAndStrip(groups, groups_size);
  const sptr result =
      executor(SYS_getgroups, (sptr)count, (sptr)groups, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(getitimer)(const SyscallExecutor &executor,
                                       int which,
                                       struct itimerval *value) {
  value = checkAndStrip(value, struct_itimerval_sz);
  const sptr result =
      executor(SYS_getitimer, (sptr)which, (sptr)value, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(getrlimit)(const SyscallExecutor &executor,
                                       int resource,
                                       struct rlimit *limit) {
  limit = checkAndStrip(limit, 2 * sizeof(unsigned long));
  const sptr result =
      executor(SYS_getrlimit, (sptr)resource, (sptr)limit, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(getrusage)(const SyscallExecutor &executor, int who,
                                       struct rusage *usage) {
  usage = checkAndStrip(usage, sizeof(struct rusage));
  const sptr result =
      executor(SYS_getrusage, (sptr)who, (sptr)usage, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(gettimeofday)(const SyscallExecutor &executor,
                                          struct timeval *time,
                                          struct timezone *timezone) {
  time = checkAndStrip(time, timeval_sz, /*optional=*/true);
  timezone = checkAndStrip(timezone, struct_timezone_sz, /*optional=*/true);
  const sptr result =
      executor(SYS_gettimeofday, (sptr)time, (sptr)timezone, 0, 0, 0, 0);
  return result;
}

#ifdef SYS_link
static sptr SYSCALL_HANDLER(link)(const SyscallExecutor &executor,
                                  const char *old_path, const char *new_path) {
  old_path = checkAndStripCString(old_path);
  new_path = checkAndStripCString(new_path);
  const sptr result =
      executor(SYS_link, (sptr)old_path, (sptr)new_path, 0, 0, 0, 0);
  return result;
}
#endif

static sptr SYSCALL_HANDLER(lremovexattr)(const SyscallExecutor &executor,
                                          const char *path, const char *name) {
  path = checkAndStripCString(path);
  name = checkAndStripCString(name);
  const sptr result =
      executor(SYS_lremovexattr, (sptr)path, (sptr)name, 0, 0, 0, 0);
  return result;
}

#ifdef SYS_lstat
static sptr SYSCALL_HANDLER(lstat)(const SyscallExecutor &executor,
                                   const char *path,
                                   struct stat *status) {
  path = checkAndStripCString(path);
  status = checkAndStrip(status, struct_kernel_stat_sz);
  const sptr result = executor(SYS_lstat, (sptr)path, (sptr)status, 0, 0, 0, 0);
  return result;
}
#endif

static sptr SYSCALL_HANDLER(memfd_create)(const SyscallExecutor &executor,
                                          const char *name, unsigned flags) {
  name = checkAndStripCString(name);
  const sptr result =
      executor(SYS_memfd_create, (sptr)name, (sptr)flags, 0, 0, 0, 0);
  return result;
}

#ifdef SYS_mkdir
static sptr SYSCALL_HANDLER(mkdir)(const SyscallExecutor &executor,
                                   const char *path,
                                   unsigned mode) {
  path = checkAndStripCString(path);
  const sptr result = executor(SYS_mkdir, (sptr)path, (sptr)mode, 0, 0, 0, 0);
  return result;
}
#endif

static sptr SYSCALL_HANDLER(pipe2)(const SyscallExecutor &executor,
                                   int *pipe_fds,
                                   int flags) {
  pipe_fds = checkAndStrip(pipe_fds, 2 * sizeof(int));
  const sptr result =
      executor(SYS_pipe2, (sptr)pipe_fds, (sptr)flags, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(pivot_root)(const SyscallExecutor &executor,
                                        const char *new_root,
                                        const char *put_old) {
  new_root = checkAndStripCString(new_root);
  put_old = checkAndStripCString(put_old);
  const sptr result =
      executor(SYS_pivot_root, (sptr)new_root, (sptr)put_old, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(removexattr)(const SyscallExecutor &executor,
                                         const char *path, const char *name) {
  path = checkAndStripCString(path);
  name = checkAndStripCString(name);
  const sptr result =
      executor(SYS_removexattr, (sptr)path, (sptr)name, 0, 0, 0, 0);
  return result;
}

#ifdef SYS_rename
static sptr SYSCALL_HANDLER(rename)(const SyscallExecutor &executor,
                                    const char *old_path,
                                    const char *new_path) {
  old_path = checkAndStripCString(old_path);
  new_path = checkAndStripCString(new_path);
  const sptr result =
      executor(SYS_rename, (sptr)old_path, (sptr)new_path, 0, 0, 0, 0);
  return result;
}
#endif

static sptr SYSCALL_HANDLER(rt_sigpending)(const SyscallExecutor &executor,
                                           void *signal_set,
                                           unsigned long signal_set_size) {
  signal_set = checkAndStrip(signal_set, sizeof(sigset_t));
  const sptr result = executor(SYS_rt_sigpending, (sptr)signal_set,
                               (sptr)signal_set_size, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(sched_getparam)(const SyscallExecutor &executor,
                                            int pid,
                                            struct sched_param *parameters) {
  // The raw syscall accesses only the sched_priority int, not the reserved
  // fields in musl's public structure.
  parameters = checkAndStrip(parameters, sizeof(int));
  const sptr result =
      executor(SYS_sched_getparam, (sptr)pid, (sptr)parameters, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(sched_rr_get_interval)(const SyscallExecutor &executor,
                                                   int pid,
                                                   struct timespec *interval) {
  interval = checkAndStrip(interval, struct_timespec_sz);
  const sptr result = executor(SYS_sched_rr_get_interval, (sptr)pid,
                               (sptr)interval, 0, 0, 0, 0);
  return result;
}

static sptr
SYSCALL_HANDLER(sched_setparam)(const SyscallExecutor &executor, int pid,
                                const struct sched_param *parameters) {
  // The raw syscall accesses only the sched_priority int, not the reserved
  // fields in musl's public structure.
  parameters = checkAndStrip(parameters, sizeof(int));
  const sptr result =
      executor(SYS_sched_setparam, (sptr)pid, (sptr)parameters, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(setdomainname)(const SyscallExecutor &executor,
                                           const char *name,
                                           unsigned long length) {
  name = checkAndStrip(name, length);
  const sptr result =
      executor(SYS_setdomainname, (sptr)name, (sptr)length, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(setgroups)(const SyscallExecutor &executor,
                                       int count,
                                       const gid_t *groups) {
  uptr groups_size = 0;
  if (count > 0) {
    groups_size = (uptr)count * sizeof(gid_t);
  }
  groups = checkAndStrip(groups, groups_size);
  const sptr result =
      executor(SYS_setgroups, (sptr)count, (sptr)groups, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(sethostname)(const SyscallExecutor &executor,
                                         const char *name,
                                         unsigned long length) {
  name = checkAndStrip(name, length);
  const sptr result =
      executor(SYS_sethostname, (sptr)name, (sptr)length, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(setrlimit)(const SyscallExecutor &executor,
                                       int resource,
                                       const struct rlimit *limit) {
  limit = checkAndStrip(limit, 2 * sizeof(unsigned long));
  const sptr result =
      executor(SYS_setrlimit, (sptr)resource, (sptr)limit, 0, 0, 0, 0);
  return result;
}

#ifdef SYS_stat
static sptr SYSCALL_HANDLER(stat)(const SyscallExecutor &executor,
                                  const char *path,
                                  struct stat *status) {
  path = checkAndStripCString(path);
  status = checkAndStrip(status, struct_kernel_stat_sz);
  const sptr result = executor(SYS_stat, (sptr)path, (sptr)status, 0, 0, 0, 0);
  return result;
}
#endif

static sptr SYSCALL_HANDLER(statfs)(const SyscallExecutor &executor,
                                    const char *path,
                                    struct statfs *status) {
  path = checkAndStripCString(path);
  status = checkAndStrip(status, struct_statfs_sz);
  const sptr result =
      executor(SYS_statfs, (sptr)path, (sptr)status, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(swapon)(const SyscallExecutor &executor,
                                    const char *path,
                                    int flags) {
  path = checkAndStripCString(path);
  const sptr result = executor(SYS_swapon, (sptr)path, (sptr)flags, 0, 0, 0, 0);
  return result;
}

#ifdef SYS_symlink
static sptr SYSCALL_HANDLER(symlink)(const SyscallExecutor &executor,
                                     const char *target,
                                     const char *link_path) {
  target = checkAndStripCString(target);
  link_path = checkAndStripCString(link_path);
  const sptr result =
      executor(SYS_symlink, (sptr)target, (sptr)link_path, 0, 0, 0, 0);
  return result;
}
#endif

static sptr SYSCALL_HANDLER(timer_gettime)(const SyscallExecutor &executor,
                                           int timer_id,
                                           struct itimerspec *value) {
  value = checkAndStrip(value, struct_itimerspec_sz);
  const sptr result =
      executor(SYS_timer_gettime, (sptr)timer_id, (sptr)value, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(timerfd_gettime)(const SyscallExecutor &executor,
                                             int fd,
                                             struct itimerspec *value) {
  value = checkAndStrip(value, struct_itimerspec_sz);
  const sptr result =
      executor(SYS_timerfd_gettime, (sptr)fd, (sptr)value, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(truncate)(const SyscallExecutor &executor,
                                      const char *path, long length) {
  path = checkAndStripCString(path);
  const sptr result =
      executor(SYS_truncate, (sptr)path, (sptr)length, 0, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(umount2)(const SyscallExecutor &executor,
                                     const char *path,
                                     int flags) {
  path = checkAndStripCString(path);
  const sptr result =
      executor(SYS_umount2, (sptr)path, (sptr)flags, 0, 0, 0, 0);
  return result;
}

#ifdef SYS_utimes
static sptr SYSCALL_HANDLER(utimes)(const SyscallExecutor &executor,
                                    const char *path,
                                    const struct timeval *times) {
  path = checkAndStripCString(path);
  times = checkAndStrip(times, 2 * timeval_sz, /*optional=*/true);
  const sptr result = executor(SYS_utimes, (sptr)path, (sptr)times, 0, 0, 0, 0);
  return result;
}
#endif

//===----------------------------------------------------------------------===//
// Audited three-argument flat syscall handlers
//===----------------------------------------------------------------------===//

#ifdef SYS_chown
static sptr SYSCALL_HANDLER(chown)(const SyscallExecutor &executor,
                                   const char *path,
                                   unsigned owner, unsigned group) {
  path = checkAndStripCString(path);
  const sptr result =
      executor(SYS_chown, (sptr)path, (sptr)owner, (sptr)group, 0, 0, 0);
  return result;
}
#endif

static sptr SYSCALL_HANDLER(faccessat)(const SyscallExecutor &executor,
                                       int dirfd,
                                       const char *path, int mode) {
  path = checkAndStripCString(path);
  const sptr result =
      executor(SYS_faccessat, (sptr)dirfd, (sptr)path, (sptr)mode, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(fchmodat)(const SyscallExecutor &executor,
                                      int dirfd,
                                      const char *path, unsigned mode) {
  path = checkAndStripCString(path);
  const sptr result =
      executor(SYS_fchmodat, (sptr)dirfd, (sptr)path, (sptr)mode, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(flistxattr)(const SyscallExecutor &executor, int fd,
                                        char *list, unsigned long size) {
  list = checkAndStrip(list, size);
  const sptr result =
      executor(SYS_flistxattr, (sptr)fd, (sptr)list, (sptr)size, 0, 0, 0);
  return result;
}

#ifdef SYS_futimesat
static sptr SYSCALL_HANDLER(futimesat)(const SyscallExecutor &executor,
                                       int dirfd,
                                       const char *path,
                                       const struct timeval *times) {
  path = checkAndStripCString(path, /*optional=*/true);
  times = checkAndStrip(times, 2 * timeval_sz, /*optional=*/true);
  const sptr result =
      executor(SYS_futimesat, (sptr)dirfd, (sptr)path, (sptr)times, 0, 0, 0);
  return result;
}
#endif

static sptr SYSCALL_HANDLER(get_robust_list)(const SyscallExecutor &executor,
                                             int pid,
                                             struct robust_list_head **head,
                                             unsigned long *length) {
  head = checkAndStrip(head, sizeof(void *));
  length = checkAndStrip(length, sizeof(uptr));
  const sptr result = executor(SYS_get_robust_list, (sptr)pid, (sptr)head,
                               (sptr)length, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(getcpu)(const SyscallExecutor &executor,
                                    unsigned *cpu,
                                    unsigned *node, void *cache) {
  cpu = checkAndStrip(cpu, sizeof(unsigned), /*optional=*/true);
  node = checkAndStrip(node, sizeof(unsigned), /*optional=*/true);
  const sptr result =
      executor(SYS_getcpu, (sptr)cpu, (sptr)node, (sptr)cache, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(getdents64)(const SyscallExecutor &executor, int fd,
                                        struct linux_dirent64 *entries,
                                        unsigned count) {
  entries = checkAndStrip(entries, count);
  const sptr result =
      executor(SYS_getdents64, (sptr)fd, (sptr)entries, (sptr)count, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(getresgid)(const SyscallExecutor &executor,
                                       unsigned *real,
                                       unsigned *effective, unsigned *saved) {
  real = checkAndStrip(real, sizeof(unsigned));
  effective = checkAndStrip(effective, sizeof(unsigned));
  saved = checkAndStrip(saved, sizeof(unsigned));
  const sptr result = executor(SYS_getresgid, (sptr)real, (sptr)effective,
                               (sptr)saved, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(getresuid)(const SyscallExecutor &executor,
                                       unsigned *real,
                                       unsigned *effective, unsigned *saved) {
  real = checkAndStrip(real, sizeof(unsigned));
  effective = checkAndStrip(effective, sizeof(unsigned));
  saved = checkAndStrip(saved, sizeof(unsigned));
  const sptr result = executor(SYS_getresuid, (sptr)real, (sptr)effective,
                               (sptr)saved, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(init_module)(const SyscallExecutor &executor,
                                         const void *image,
                                         unsigned long length,
                                         const char *parameters) {
  image = checkAndStrip(image, length);
  parameters = checkAndStripCString(parameters);
  const sptr result = executor(SYS_init_module, (sptr)image, (sptr)length,
                               (sptr)parameters, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(inotify_add_watch)(const SyscallExecutor &executor,
                                               int fd,
                                               const char *path,
                                               unsigned mask) {
  path = checkAndStripCString(path);
  const sptr result = executor(SYS_inotify_add_watch, (sptr)fd, (sptr)path,
                               (sptr)mask, 0, 0, 0);
  return result;
}

#ifdef SYS_lchown
static sptr SYSCALL_HANDLER(lchown)(const SyscallExecutor &executor,
                                    const char *path,
                                    unsigned owner, unsigned group) {
  path = checkAndStripCString(path);
  const sptr result =
      executor(SYS_lchown, (sptr)path, (sptr)owner, (sptr)group, 0, 0, 0);
  return result;
}
#endif

static sptr SYSCALL_HANDLER(listxattr)(const SyscallExecutor &executor,
                                       const char *path, char *list,
                                       unsigned long size) {
  path = checkAndStripCString(path);
  list = checkAndStrip(list, size);
  const sptr result =
      executor(SYS_listxattr, (sptr)path, (sptr)list, (sptr)size, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(llistxattr)(const SyscallExecutor &executor,
                                        const char *path, char *list,
                                        unsigned long size) {
  path = checkAndStripCString(path);
  list = checkAndStrip(list, size);
  const sptr result =
      executor(SYS_llistxattr, (sptr)path, (sptr)list, (sptr)size, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(mkdirat)(const SyscallExecutor &executor, int dirfd,
                                     const char *path, unsigned mode) {
  path = checkAndStripCString(path);
  const sptr result =
      executor(SYS_mkdirat, (sptr)dirfd, (sptr)path, (sptr)mode, 0, 0, 0);
  return result;
}

#ifdef SYS_mknod
static sptr SYSCALL_HANDLER(mknod)(const SyscallExecutor &executor,
                                   const char *path,
                                   unsigned mode, unsigned long device) {
  path = checkAndStripCString(path);
  const sptr result =
      executor(SYS_mknod, (sptr)path, (sptr)mode, (sptr)device, 0, 0, 0);
  return result;
}
#endif

static sptr SYSCALL_HANDLER(mq_getsetattr)(const SyscallExecutor &executor,
                                           int mqdes,
                                           const struct mq_attr *new_attributes,
                                           struct mq_attr *old_attributes) {
  new_attributes = checkAndStrip(new_attributes, struct_mq_attr_sz,
                                 /*optional=*/true);
  old_attributes = checkAndStrip(old_attributes, struct_mq_attr_sz,
                                 /*optional=*/true);
  const sptr result =
      executor(SYS_mq_getsetattr, (sptr)mqdes, (sptr)new_attributes,
               (sptr)old_attributes, 0, 0, 0);
  return result;
}

#ifdef SYS_readlink
static sptr SYSCALL_HANDLER(readlink)(const SyscallExecutor &executor,
                                      const char *path, char *buffer,
                                      unsigned long size) {
  path = checkAndStripCString(path);
  buffer = checkAndStrip(buffer, size);
  const sptr result =
      executor(SYS_readlink, (sptr)path, (sptr)buffer, (sptr)size, 0, 0, 0);
  return result;
}
#endif

static sptr SYSCALL_HANDLER(sched_getaffinity)(const SyscallExecutor &executor,
                                               int pid, unsigned long size,
                                               void *mask) {
  mask = checkAndStrip(mask, size);
  const sptr result = executor(SYS_sched_getaffinity, (sptr)pid, (sptr)size,
                               (sptr)mask, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(sched_setaffinity)(const SyscallExecutor &executor,
                                               int pid, unsigned long size,
                                               const void *mask) {
  mask = checkAndStrip(mask, size);
  const sptr result = executor(SYS_sched_setaffinity, (sptr)pid, (sptr)size,
                               (sptr)mask, 0, 0, 0);
  return result;
}

static sptr
SYSCALL_HANDLER(sched_setscheduler)(const SyscallExecutor &executor, int pid,
                                    int policy,
                                    const struct sched_param *parameters) {
  // The raw syscall accesses only the sched_priority int, not the reserved
  // fields in musl's public structure.
  parameters = checkAndStrip(parameters, sizeof(int));
  const sptr result = executor(SYS_sched_setscheduler, (sptr)pid, (sptr)policy,
                               (sptr)parameters, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(semop)(const SyscallExecutor &executor, int semid,
                                   const struct sembuf *operations,
                                   unsigned long count) {
  operations = checkAndStrip(operations,
                             saturatingMultiply(count, sizeof(struct sembuf)));
  const sptr result =
      executor(SYS_semop, (sptr)semid, (sptr)operations, (sptr)count, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(setitimer)(const SyscallExecutor &executor,
                                       int which,
                                       const struct itimerval *new_value,
                                       struct itimerval *old_value) {
  new_value = checkAndStrip(new_value, struct_itimerval_sz, /*optional=*/true);
  old_value = checkAndStrip(old_value, struct_itimerval_sz, /*optional=*/true);
  const sptr result = executor(SYS_setitimer, (sptr)which, (sptr)new_value,
                               (sptr)old_value, 0, 0, 0);
  return result;
}

#ifdef SYS_signalfd
static sptr SYSCALL_HANDLER(signalfd)(const SyscallExecutor &executor, int fd,
                                      const void *signal_mask,
                                      unsigned long signal_mask_size) {
  signal_mask = checkAndStrip(signal_mask, sizeof(sigset_t));
  const sptr result = executor(SYS_signalfd, (sptr)fd, (sptr)signal_mask,
                               (sptr)signal_mask_size, 0, 0, 0);
  return result;
}
#endif

static sptr SYSCALL_HANDLER(symlinkat)(const SyscallExecutor &executor,
                                       const char *target, int new_dirfd,
                                       const char *link_path) {
  target = checkAndStripCString(target);
  link_path = checkAndStripCString(link_path);
  const sptr result = executor(SYS_symlinkat, (sptr)target, (sptr)new_dirfd,
                               (sptr)link_path, 0, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(unlinkat)(const SyscallExecutor &executor,
                                      int dirfd,
                                      const char *path, int flags) {
  path = checkAndStripCString(path);
  const sptr result =
      executor(SYS_unlinkat, (sptr)dirfd, (sptr)path, (sptr)flags, 0, 0, 0);
  return result;
}

//===----------------------------------------------------------------------===//
// Audited four-argument flat syscall handlers
//===----------------------------------------------------------------------===//

static sptr SYSCALL_HANDLER(faccessat2)(const SyscallExecutor &executor,
                                        int dirfd,
                                        const char *path, int mode, int flags) {
  path = checkAndStripCString(path);
  const sptr result = executor(SYS_faccessat2, (sptr)dirfd, (sptr)path,
                               (sptr)mode, (sptr)flags, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(fchmodat2)(const SyscallExecutor &executor,
                                       int dirfd,
                                       const char *path, unsigned mode,
                                       int flags) {
  path = checkAndStripCString(path);
  const sptr result = executor(SYS_fchmodat2, (sptr)dirfd, (sptr)path,
                               (sptr)mode, (sptr)flags, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(fgetxattr)(const SyscallExecutor &executor, int fd,
                                       const char *name, void *value,
                                       unsigned long size) {
  name = checkAndStripCString(name);
  value = checkAndStrip(value, size);
  const sptr result = executor(SYS_fgetxattr, (sptr)fd, (sptr)name, (sptr)value,
                               (sptr)size, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(newfstatat)(const SyscallExecutor &executor,
                                        int dirfd,
                                        const char *path, struct stat *status,
                                        int flags) {
  path = checkAndStripCString(path);
  status = checkAndStrip(status, struct_kernel_stat_sz);
  const sptr result = executor(SYS_newfstatat, (sptr)dirfd, (sptr)path,
                               (sptr)status, (sptr)flags, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(getxattr)(const SyscallExecutor &executor,
                                      const char *path, const char *name,
                                      void *value, unsigned long size) {
  path = checkAndStripCString(path);
  name = checkAndStripCString(name);
  value = checkAndStrip(value, size);
  const sptr result = executor(SYS_getxattr, (sptr)path, (sptr)name,
                               (sptr)value, (sptr)size, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(lgetxattr)(const SyscallExecutor &executor,
                                       const char *path, const char *name,
                                       void *value, unsigned long size) {
  path = checkAndStripCString(path);
  name = checkAndStripCString(name);
  value = checkAndStrip(value, size);
  const sptr result = executor(SYS_lgetxattr, (sptr)path, (sptr)name,
                               (sptr)value, (sptr)size, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(mknodat)(const SyscallExecutor &executor, int dirfd,
                                     const char *path, unsigned mode,
                                     unsigned long device) {
  path = checkAndStripCString(path);
  const sptr result = executor(SYS_mknodat, (sptr)dirfd, (sptr)path, (sptr)mode,
                               (sptr)device, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(mq_open)(const SyscallExecutor &executor,
                                     const char *name,
                                     int flags, unsigned mode,
                                     const struct mq_attr *attributes) {
  name = checkAndStripCString(name);
  attributes = checkAndStrip(attributes, struct_mq_attr_sz, /*optional=*/true);
  const sptr result = executor(SYS_mq_open, (sptr)name, (sptr)flags, (sptr)mode,
                               (sptr)attributes, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(prlimit64)(const SyscallExecutor &executor, int pid,
                                       int resource,
                                       const struct rlimit *new_limit,
                                       struct rlimit *old_limit) {
  new_limit = checkAndStrip(new_limit, 2 * sizeof(u64), /*optional=*/true);
  old_limit = checkAndStrip(old_limit, 2 * sizeof(u64), /*optional=*/true);
  const sptr result = executor(SYS_prlimit64, (sptr)pid, (sptr)resource,
                               (sptr)new_limit, (sptr)old_limit, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(readlinkat)(const SyscallExecutor &executor,
                                        int dirfd,
                                        const char *path, char *buffer,
                                        unsigned long size) {
  path = checkAndStripCString(path);
  buffer = checkAndStrip(buffer, size);
  const sptr result = executor(SYS_readlinkat, (sptr)dirfd, (sptr)path,
                               (sptr)buffer, (sptr)size, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(renameat)(const SyscallExecutor &executor,
                                      int old_dirfd,
                                      const char *old_path, int new_dirfd,
                                      const char *new_path) {
  old_path = checkAndStripCString(old_path);
  new_path = checkAndStripCString(new_path);
  const sptr result = executor(SYS_renameat, (sptr)old_dirfd, (sptr)old_path,
                               (sptr)new_dirfd, (sptr)new_path, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(rt_sigprocmask)(const SyscallExecutor &executor,
                                            int how,
                                            const void *signal_set,
                                            void *old_signal_set,
                                            unsigned long signal_set_size) {
  signal_set = checkAndStrip(signal_set, signal_set_size, /*optional=*/true);
  old_signal_set = checkAndStrip(old_signal_set, signal_set_size,
                                 /*optional=*/true);
  const sptr result =
      executor(SYS_rt_sigprocmask, (sptr)how, (sptr)signal_set,
               (sptr)old_signal_set, (sptr)signal_set_size, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(semtimedop)(const SyscallExecutor &executor,
                                        int semid,
                                        const struct sembuf *operations,
                                        unsigned long count,
                                        const struct timespec *timeout) {
  operations = checkAndStrip(operations,
                             saturatingMultiply(count, sizeof(struct sembuf)));
  timeout = checkAndStrip(timeout, struct_timespec_sz, /*optional=*/true);
  const sptr result = executor(SYS_semtimedop, (sptr)semid, (sptr)operations,
                               (sptr)count, (sptr)timeout, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(sendfile)(const SyscallExecutor &executor,
                                      int output_fd,
                                      int input_fd, long *offset,
                                      unsigned long count) {
  offset = checkAndStrip(offset, sizeof(s64),
                         /*optional=*/true);
  const sptr result = executor(SYS_sendfile, (sptr)output_fd, (sptr)input_fd,
                               (sptr)offset, (sptr)count, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(signalfd4)(const SyscallExecutor &executor, int fd,
                                       const void *signal_mask,
                                       unsigned long signal_mask_size,
                                       int flags) {
  signal_mask = checkAndStrip(signal_mask, sizeof(sigset_t));
  const sptr result = executor(SYS_signalfd4, (sptr)fd, (sptr)signal_mask,
                               (sptr)signal_mask_size, (sptr)flags, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(timer_settime)(const SyscallExecutor &executor,
                                           int timer_id, int flags,
                                           const struct itimerspec *new_value,
                                           struct itimerspec *old_value) {
  new_value = checkAndStrip(new_value, struct_itimerspec_sz);
  old_value = checkAndStrip(old_value, struct_itimerspec_sz,
                            /*optional=*/true);
  const sptr result = executor(SYS_timer_settime, (sptr)timer_id, (sptr)flags,
                               (sptr)new_value, (sptr)old_value, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(timerfd_settime)(const SyscallExecutor &executor,
                                             int fd,
                                             int flags,
                                             const struct itimerspec *new_value,
                                             struct itimerspec *old_value) {
  new_value = checkAndStrip(new_value, struct_itimerspec_sz);
  old_value = checkAndStrip(old_value, struct_itimerspec_sz,
                            /*optional=*/true);
  const sptr result = executor(SYS_timerfd_settime, (sptr)fd, (sptr)flags,
                               (sptr)new_value, (sptr)old_value, 0, 0);
  return result;
}

static sptr SYSCALL_HANDLER(utimensat)(const SyscallExecutor &executor,
                                       int dirfd,
                                       const char *path,
                                       const struct timespec *times,
                                       int flags) {
  path = checkAndStripCString(path, /*optional=*/true);
  times = checkAndStrip(times, 2 * struct_timespec_sz, /*optional=*/true);
  const sptr result = executor(SYS_utimensat, (sptr)dirfd, (sptr)path,
                               (sptr)times, (sptr)flags, 0, 0);
  return result;
}

//===----------------------------------------------------------------------===//
// Audited five-argument flat syscall handlers
//===----------------------------------------------------------------------===//

static sptr SYSCALL_HANDLER(fanotify_mark)(const SyscallExecutor &executor,
                                           int fanotify_fd, unsigned flags,
                                           unsigned long long mask, int dirfd,
                                           const char *path) {
  if (flags & FAN_MARK_FLUSH) {
    path = clear_id(path);
  } else {
    path = checkAndStripCString(path);
  }
  const sptr result =
      executor(SYS_fanotify_mark, (sptr)fanotify_fd, (sptr)flags, (sptr)mask,
               (sptr)dirfd, (sptr)path, 0);
  return result;
}

static sptr SYSCALL_HANDLER(fchownat)(const SyscallExecutor &executor,
                                      int dirfd,
                                      const char *path, unsigned owner,
                                      unsigned group, int flags) {
  path = checkAndStripCString(path);
  const sptr result = executor(SYS_fchownat, (sptr)dirfd, (sptr)path,
                               (sptr)owner, (sptr)group, (sptr)flags, 0);
  return result;
}

static sptr SYSCALL_HANDLER(fsetxattr)(const SyscallExecutor &executor, int fd,
                                       const char *name, const void *value,
                                       unsigned long size, int flags) {
  name = checkAndStripCString(name);
  value = checkAndStrip(value, size);
  const sptr result = executor(SYS_fsetxattr, (sptr)fd, (sptr)name, (sptr)value,
                               (sptr)size, (sptr)flags, 0);
  return result;
}

static sptr SYSCALL_HANDLER(linkat)(const SyscallExecutor &executor,
                                    int old_dirfd,
                                    const char *old_path, int new_dirfd,
                                    const char *new_path, int flags) {
  old_path = checkAndStripCString(old_path);
  new_path = checkAndStripCString(new_path);
  const sptr result = executor(SYS_linkat, (sptr)old_dirfd, (sptr)old_path,
                               (sptr)new_dirfd, (sptr)new_path, (sptr)flags, 0);
  return result;
}

static sptr SYSCALL_HANDLER(lsetxattr)(const SyscallExecutor &executor,
                                       const char *path, const char *name,
                                       const void *value, unsigned long size,
                                       int flags) {
  path = checkAndStripCString(path);
  name = checkAndStripCString(name);
  value = checkAndStrip(value, size);
  const sptr result = executor(SYS_lsetxattr, (sptr)path, (sptr)name,
                               (sptr)value, (sptr)size, (sptr)flags, 0);
  return result;
}

static sptr SYSCALL_HANDLER(renameat2)(const SyscallExecutor &executor,
                                       int old_dirfd,
                                       const char *old_path, int new_dirfd,
                                       const char *new_path, unsigned flags) {
  old_path = checkAndStripCString(old_path);
  new_path = checkAndStripCString(new_path);
  const sptr result = executor(SYS_renameat2, (sptr)old_dirfd, (sptr)old_path,
                               (sptr)new_dirfd, (sptr)new_path, (sptr)flags, 0);
  return result;
}

static sptr SYSCALL_HANDLER(setxattr)(const SyscallExecutor &executor,
                                      const char *path, const char *name,
                                      const void *value, unsigned long size,
                                      int flags) {
  path = checkAndStripCString(path);
  name = checkAndStripCString(name);
  value = checkAndStrip(value, size);
  const sptr result = executor(SYS_setxattr, (sptr)path, (sptr)name,
                               (sptr)value, (sptr)size, (sptr)flags, 0);
  return result;
}

static sptr SYSCALL_HANDLER(statx)(const SyscallExecutor &executor, int dirfd,
                                   const char *path, int flags, unsigned mask,
                                   struct statx *status) {
  path = checkAndStripCString(path);
  status = checkAndStrip(status, sizeof(struct statx));
  const sptr result = executor(SYS_statx, (sptr)dirfd, (sptr)path, (sptr)flags,
                               (sptr)mask, (sptr)status, 0);
  return result;
}

//===----------------------------------------------------------------------===//
// Socket-address and paired-offset syscall handlers
//===----------------------------------------------------------------------===//

static sptr SYSCALL_HANDLER(bind)(const SyscallExecutor &executor,
                                  int socket_fd,
                                  const struct sockaddr *address,
                                  unsigned address_length) {
  address = checkAndStrip(address, address_length);
  const sptr result = executor(SYS_bind, (sptr)socket_fd, (sptr)address,
                               (sptr)address_length, (sptr)0, (sptr)0, (sptr)0);
  return result;
}

static sptr
SYSCALL_HANDLER(copy_file_range)(const SyscallExecutor &executor, int input_fd,
                                 long long *input_offset, int output_fd,
                                 long long *output_offset, unsigned long length,
                                 unsigned flags) {
  input_offset = checkAndStrip(input_offset, sizeof(s64),
                               /*optional=*/true);
  output_offset = checkAndStrip(output_offset, sizeof(s64),
                                /*optional=*/true);
  const sptr result =
      executor(SYS_copy_file_range, (sptr)input_fd, (sptr)input_offset,
               (sptr)output_fd, (sptr)output_offset, (sptr)length, (sptr)flags);
  return result;
}

static sptr SYSCALL_HANDLER(getpeername)(const SyscallExecutor &executor,
                                         int socket_fd,
                                         struct sockaddr *address,
                                         unsigned *address_length) {
  uptr addressSize = readSocketLengthForValidation(address_length);
  address = checkAndStrip(address, addressSize);
  address_length = clear_id(address_length);
  const sptr result = executor(SYS_getpeername, (sptr)socket_fd, (sptr)address,
                               (sptr)address_length, (sptr)0, (sptr)0, (sptr)0);
  return result;
}

static sptr SYSCALL_HANDLER(getsockname)(const SyscallExecutor &executor,
                                         int socket_fd,
                                         struct sockaddr *address,
                                         unsigned *address_length) {
  uptr addressSize = readSocketLengthForValidation(address_length);
  address = checkAndStrip(address, addressSize);
  address_length = clear_id(address_length);
  const sptr result = executor(SYS_getsockname, (sptr)socket_fd, (sptr)address,
                               (sptr)address_length, (sptr)0, (sptr)0, (sptr)0);
  return result;
}

static sptr SYSCALL_HANDLER(socketpair)(const SyscallExecutor &executor,
                                        int domain,
                                        int type, int protocol, int *sockets) {
  sockets = checkAndStrip(sockets, 2 * sizeof(int));
  const sptr result = executor(SYS_socketpair, (sptr)domain, (sptr)type,
                               (sptr)protocol, (sptr)sockets, (sptr)0, (sptr)0);
  return result;
}

static sptr SYSCALL_HANDLER(splice)(const SyscallExecutor &executor,
                                    int input_fd,
                                    long long *input_offset, int output_fd,
                                    long long *output_offset,
                                    unsigned long length, unsigned flags) {
  input_offset = checkAndStrip(input_offset, sizeof(s64),
                               /*optional=*/true);
  output_offset = checkAndStrip(output_offset, sizeof(s64),
                                /*optional=*/true);
  const sptr result =
      executor(SYS_splice, (sptr)input_fd, (sptr)input_offset, (sptr)output_fd,
               (sptr)output_offset, (sptr)length, (sptr)flags);
  return result;
}

//===----------------------------------------------------------------------===//
// Central syscall dispatcher
//===----------------------------------------------------------------------===//

static sptr dispatchSyscall(const SyscallExecutor &executor,
                            sptr number, sptr a,
                            sptr b, sptr c, sptr d, sptr e, sptr f) {
  if (!ptsan_initialized) {
    return executor(number, a, b, c, d, e, f);
  }

  switch (number) {
  case SYS_exit:
    FinishMuslThread();
    // musl's __pthread_exit already retries SYS_exit in an infinite loop.
    // Keep that loop in the uninstrumented runtime: after retiring stack/TLS
    // IDs, we must not return to instrumented musl if the syscall is denied.
    // SYS_exit_group bypasses per-thread cleanup and leaves other threads'
    // metadata intact.
    for (;;)
      executor(number, a, b, c, d, e, f);
  // Audited one-argument flat syscalls.
  case SYS_acct:
    return SYSCALL_HANDLER(acct)(executor, (const char *)a);
  case SYS_adjtimex:
    return SYSCALL_HANDLER(adjtimex)(executor, (struct timex *)a);
  case SYS_chdir:
    return SYSCALL_HANDLER(chdir)(executor, (const char *)a);
  case SYS_chroot:
    return SYSCALL_HANDLER(chroot)(executor, (const char *)a);
  case SYS_mq_unlink:
    return SYSCALL_HANDLER(mq_unlink)(executor, (const char *)a);
#ifdef SYS_pipe
  case SYS_pipe:
    return SYSCALL_HANDLER(pipe)(executor, (int *)a);
#endif
#ifdef SYS_rmdir
  case SYS_rmdir:
    return SYSCALL_HANDLER(rmdir)(executor, (const char *)a);
#endif
  case SYS_swapoff:
    return SYSCALL_HANDLER(swapoff)(executor, (const char *)a);
  case SYS_sysinfo:
    return SYSCALL_HANDLER(sysinfo)(executor, (struct sysinfo *)a);
  case SYS_times:
    return SYSCALL_HANDLER(times)(executor, (struct tms *)a);
  case SYS_uname:
    return SYSCALL_HANDLER(uname)(executor, (void *)a);
#ifdef SYS_unlink
  case SYS_unlink:
    return SYSCALL_HANDLER(unlink)(executor, (const char *)a);
#endif
  // Audited two-argument flat syscalls.
#ifdef SYS_access
  case SYS_access:
    return SYSCALL_HANDLER(access)(executor, (const char *)a, (int)b);
#endif
  case SYS_capget:
    return SYSCALL_HANDLER(capget)(executor, (void *)a, (void *)b);
  case SYS_capset:
    return SYSCALL_HANDLER(capset)(executor, (const void *)a, (const void *)b);
#ifdef SYS_chmod
  case SYS_chmod:
    return SYSCALL_HANDLER(chmod)(executor, (const char *)a, (unsigned)b);
#endif
  case SYS_clock_adjtime:
    return SYSCALL_HANDLER(clock_adjtime)(executor, (int)a, (struct timex *)b);
  case SYS_clock_getres:
    return SYSCALL_HANDLER(clock_getres)(executor, (int)a,
                                         (struct timespec *)b);
  case SYS_clock_gettime:
    return SYSCALL_HANDLER(clock_gettime)(executor, (int)a,
                                          (struct timespec *)b);
  case SYS_clock_settime:
    return SYSCALL_HANDLER(clock_settime)(executor, (int)a,
                                          (const struct timespec *)b);
  case SYS_delete_module:
    return SYSCALL_HANDLER(delete_module)(executor, (const char *)a, (int)b);
  case SYS_fremovexattr:
    return SYSCALL_HANDLER(fremovexattr)(executor, (int)a, (const char *)b);
  case SYS_fstat:
    return SYSCALL_HANDLER(fstat)(executor, (int)a, (struct stat *)b);
  case SYS_fstatfs:
    return SYSCALL_HANDLER(fstatfs)(executor, (int)a, (struct statfs *)b);
  case SYS_getcwd:
    return SYSCALL_HANDLER(getcwd)(executor, (char *)a, (unsigned long)b);
  case SYS_getgroups:
    return SYSCALL_HANDLER(getgroups)(executor, (int)a, (gid_t *)b);
  case SYS_getitimer:
    return SYSCALL_HANDLER(getitimer)(executor, (int)a, (struct itimerval *)b);
  case SYS_getrlimit:
    return SYSCALL_HANDLER(getrlimit)(executor, (int)a, (struct rlimit *)b);
  case SYS_getrusage:
    return SYSCALL_HANDLER(getrusage)(executor, (int)a, (struct rusage *)b);
  case SYS_gettimeofday:
    return SYSCALL_HANDLER(gettimeofday)(executor, (struct timeval *)a,
                                         (struct timezone *)b);
#ifdef SYS_link
  case SYS_link:
    return SYSCALL_HANDLER(link)(executor, (const char *)a, (const char *)b);
#endif
  case SYS_lremovexattr:
    return SYSCALL_HANDLER(lremovexattr)(executor, (const char *)a,
                                         (const char *)b);
#ifdef SYS_lstat
  case SYS_lstat:
    return SYSCALL_HANDLER(lstat)(executor, (const char *)a, (struct stat *)b);
#endif
  case SYS_memfd_create:
    return SYSCALL_HANDLER(memfd_create)(executor, (const char *)a,
                                         (unsigned)b);
#ifdef SYS_mkdir
  case SYS_mkdir:
    return SYSCALL_HANDLER(mkdir)(executor, (const char *)a, (unsigned)b);
#endif
#ifdef SYS_open
  case SYS_open:
    return SYSCALL_HANDLER(open)(executor, (const char *)a, (int)b,
                                 (unsigned)c);
#endif
  case SYS_pipe2:
    return SYSCALL_HANDLER(pipe2)(executor, (int *)a, (int)b);
  case SYS_pivot_root:
    return SYSCALL_HANDLER(pivot_root)(executor, (const char *)a,
                                       (const char *)b);
  case SYS_removexattr:
    return SYSCALL_HANDLER(removexattr)(executor, (const char *)a,
                                        (const char *)b);
#ifdef SYS_rename
  case SYS_rename:
    return SYSCALL_HANDLER(rename)(executor, (const char *)a, (const char *)b);
#endif
  case SYS_rt_sigpending:
    return SYSCALL_HANDLER(rt_sigpending)(executor, (void *)a,
                                          (unsigned long)b);
  case SYS_sched_getparam:
    return SYSCALL_HANDLER(sched_getparam)(executor, (int)a,
                                           (struct sched_param *)b);
  case SYS_sched_rr_get_interval:
    return SYSCALL_HANDLER(sched_rr_get_interval)(executor, (int)a,
                                                  (struct timespec *)b);
  case SYS_sched_setparam:
    return SYSCALL_HANDLER(sched_setparam)(executor, (int)a,
                                           (const struct sched_param *)b);
  case SYS_setdomainname:
    return SYSCALL_HANDLER(setdomainname)(executor, (const char *)a,
                                          (unsigned long)b);
  case SYS_setgroups:
    return SYSCALL_HANDLER(setgroups)(executor, (int)a, (const gid_t *)b);
  case SYS_sethostname:
    return SYSCALL_HANDLER(sethostname)(executor, (const char *)a,
                                        (unsigned long)b);
  case SYS_setrlimit:
    return SYSCALL_HANDLER(setrlimit)(executor, (int)a,
                                      (const struct rlimit *)b);
#ifdef SYS_stat
  case SYS_stat:
    return SYSCALL_HANDLER(stat)(executor, (const char *)a, (struct stat *)b);
#endif
  case SYS_statfs:
    return SYSCALL_HANDLER(statfs)(executor, (const char *)a,
                                   (struct statfs *)b);
  case SYS_swapon:
    return SYSCALL_HANDLER(swapon)(executor, (const char *)a, (int)b);
#ifdef SYS_symlink
  case SYS_symlink:
    return SYSCALL_HANDLER(symlink)(executor, (const char *)a, (const char *)b);
#endif
  case SYS_timer_gettime:
    return SYSCALL_HANDLER(timer_gettime)(executor, (int)a,
                                          (struct itimerspec *)b);
  case SYS_timerfd_gettime:
    return SYSCALL_HANDLER(timerfd_gettime)(executor, (int)a,
                                            (struct itimerspec *)b);
  case SYS_truncate:
    return SYSCALL_HANDLER(truncate)(executor, (const char *)a, (long)b);
  case SYS_umount2:
    return SYSCALL_HANDLER(umount2)(executor, (const char *)a, (int)b);
#ifdef SYS_utimes
  case SYS_utimes:
    return SYSCALL_HANDLER(utimes)(executor, (const char *)a,
                                   (const struct timeval *)b);
#endif
  // Audited three-argument flat syscalls.
#ifdef SYS_chown
  case SYS_chown:
    return SYSCALL_HANDLER(chown)(executor, (const char *)a, (unsigned)b,
                                  (unsigned)c);
#endif
  case SYS_faccessat:
    return SYSCALL_HANDLER(faccessat)(executor, (int)a, (const char *)b,
                                      (int)c);
  case SYS_fchmodat:
    return SYSCALL_HANDLER(fchmodat)(executor, (int)a, (const char *)b,
                                     (unsigned)c);
  case SYS_flistxattr:
    return SYSCALL_HANDLER(flistxattr)(executor, (int)a, (char *)b,
                                       (unsigned long)c);
#ifdef SYS_futimesat
  case SYS_futimesat:
    return SYSCALL_HANDLER(futimesat)(executor, (int)a, (const char *)b,
                                      (const struct timeval *)c);
#endif
  case SYS_get_robust_list:
    return SYSCALL_HANDLER(get_robust_list)(
        executor, (int)a, (struct robust_list_head **)b, (unsigned long *)c);
  case SYS_getcpu:
    return SYSCALL_HANDLER(getcpu)(executor, (unsigned *)a, (unsigned *)b,
                                   (void *)c);
  case SYS_getdents64:
    return SYSCALL_HANDLER(getdents64)(executor, (int)a,
                                       (struct linux_dirent64 *)b, (unsigned)c);
  case SYS_getresgid:
    return SYSCALL_HANDLER(getresgid)(executor, (unsigned *)a, (unsigned *)b,
                                      (unsigned *)c);
  case SYS_getresuid:
    return SYSCALL_HANDLER(getresuid)(executor, (unsigned *)a, (unsigned *)b,
                                      (unsigned *)c);
  case SYS_init_module:
    return SYSCALL_HANDLER(init_module)(executor, (const void *)a,
                                        (unsigned long)b, (const char *)c);
  case SYS_inotify_add_watch:
    return SYSCALL_HANDLER(inotify_add_watch)(executor, (int)a, (const char *)b,
                                              (unsigned)c);
#ifdef SYS_lchown
  case SYS_lchown:
    return SYSCALL_HANDLER(lchown)(executor, (const char *)a, (unsigned)b,
                                   (unsigned)c);
#endif
  case SYS_listxattr:
    return SYSCALL_HANDLER(listxattr)(executor, (const char *)a, (char *)b,
                                      (unsigned long)c);
  case SYS_llistxattr:
    return SYSCALL_HANDLER(llistxattr)(executor, (const char *)a, (char *)b,
                                       (unsigned long)c);
  case SYS_mkdirat:
    return SYSCALL_HANDLER(mkdirat)(executor, (int)a, (const char *)b,
                                    (unsigned)c);
#ifdef SYS_mknod
  case SYS_mknod:
    return SYSCALL_HANDLER(mknod)(executor, (const char *)a, (unsigned)b,
                                  (unsigned long)c);
#endif
  case SYS_mq_getsetattr:
    return SYSCALL_HANDLER(mq_getsetattr)(
        executor, (int)a, (const struct mq_attr *)b, (struct mq_attr *)c);
  case SYS_mincore:
    return SYSCALL_HANDLER(mincore)(executor, (void *)a, (unsigned long)b,
                                    (unsigned char *)c);
  case SYS_open_by_handle_at:
    return SYSCALL_HANDLER(open_by_handle_at)(
        executor, (int)a, (__sanitizer_file_handle *)b, (int)c);
  case SYS_openat:
    return SYSCALL_HANDLER(openat)(executor, (int)a, (const char *)b, (int)c,
                                   (unsigned)d);
#ifdef SYS_poll
  case SYS_poll:
    return SYSCALL_HANDLER(poll)(executor, (struct pollfd *)a, (unsigned long)b,
                                 (int)c);
#endif
  case SYS_read:
    return SYSCALL_HANDLER(read)(executor, (int)a, (void *)b, (unsigned long)c);
#ifdef SYS_readlink
  case SYS_readlink:
    return SYSCALL_HANDLER(readlink)(executor, (const char *)a, (char *)b,
                                     (unsigned long)c);
#endif
  case SYS_sched_getaffinity:
    return SYSCALL_HANDLER(sched_getaffinity)(executor, (int)a,
                                              (unsigned long)b, (void *)c);
  case SYS_sched_setaffinity:
    return SYSCALL_HANDLER(sched_setaffinity)(
        executor, (int)a, (unsigned long)b, (const void *)c);
  case SYS_sched_setscheduler:
    return SYSCALL_HANDLER(sched_setscheduler)(executor, (int)a, (int)b,
                                               (const struct sched_param *)c);
  case SYS_semop:
    return SYSCALL_HANDLER(semop)(executor, (int)a, (const struct sembuf *)b,
                                  (unsigned long)c);
  case SYS_setitimer:
    return SYSCALL_HANDLER(setitimer)(
        executor, (int)a, (const struct itimerval *)b, (struct itimerval *)c);
#ifdef SYS_signalfd
  case SYS_signalfd:
    return SYSCALL_HANDLER(signalfd)(executor, (int)a, (const void *)b,
                                     (unsigned long)c);
#endif
  case SYS_symlinkat:
    return SYSCALL_HANDLER(symlinkat)(executor, (const char *)a, (int)b,
                                      (const char *)c);
  case SYS_unlinkat:
    return SYSCALL_HANDLER(unlinkat)(executor, (int)a, (const char *)b, (int)c);
  case SYS_write:
    return SYSCALL_HANDLER(write)(executor, (int)a, (const void *)b,
                                  (unsigned long)c);
  // Audited four-argument flat syscalls.
  case SYS_faccessat2:
    return SYSCALL_HANDLER(faccessat2)(executor, (int)a, (const char *)b,
                                       (int)c, (int)d);
  case SYS_fchmodat2:
    return SYSCALL_HANDLER(fchmodat2)(executor, (int)a, (const char *)b,
                                      (unsigned)c, (int)d);
  case SYS_fgetxattr:
    return SYSCALL_HANDLER(fgetxattr)(executor, (int)a, (const char *)b,
                                      (void *)c, (unsigned long)d);
  case SYS_newfstatat:
    return SYSCALL_HANDLER(newfstatat)(executor, (int)a, (const char *)b,
                                       (struct stat *)c, (int)d);
  case SYS_getxattr:
    return SYSCALL_HANDLER(getxattr)(executor, (const char *)a, (const char *)b,
                                     (void *)c, (unsigned long)d);
  case SYS_lgetxattr:
    return SYSCALL_HANDLER(lgetxattr)(executor, (const char *)a,
                                      (const char *)b, (void *)c,
                                      (unsigned long)d);
  case SYS_mknodat:
    return SYSCALL_HANDLER(mknodat)(executor, (int)a, (const char *)b,
                                    (unsigned)c, (unsigned long)d);
  case SYS_mq_open:
    return SYSCALL_HANDLER(mq_open)(executor, (const char *)a, (int)b,
                                    (unsigned)c, (const struct mq_attr *)d);
  case SYS_prlimit64:
    return SYSCALL_HANDLER(prlimit64)(
        executor, (int)a, (int)b, (const struct rlimit *)c, (struct rlimit *)d);
  case SYS_readlinkat:
    return SYSCALL_HANDLER(readlinkat)(executor, (int)a, (const char *)b,
                                       (char *)c, (unsigned long)d);
  case SYS_renameat:
    return SYSCALL_HANDLER(renameat)(executor, (int)a, (const char *)b, (int)c,
                                     (const char *)d);
  case SYS_rt_sigprocmask:
    return SYSCALL_HANDLER(rt_sigprocmask)(executor, (int)a, (const void *)b,
                                           (void *)c, (unsigned long)d);
  case SYS_semtimedop:
    return SYSCALL_HANDLER(semtimedop)(
        executor, (int)a, (const struct sembuf *)b, (unsigned long)c,
        (const struct timespec *)d);
  case SYS_sendfile:
    return SYSCALL_HANDLER(sendfile)(executor, (int)a, (int)b, (long *)c,
                                     (unsigned long)d);
  case SYS_signalfd4:
    return SYSCALL_HANDLER(signalfd4)(executor, (int)a, (const void *)b,
                                      (unsigned long)c, (int)d);
  case SYS_timer_settime:
    return SYSCALL_HANDLER(timer_settime)(executor, (int)a, (int)b,
                                          (const struct itimerspec *)c,
                                          (struct itimerspec *)d);
  case SYS_timerfd_settime:
    return SYSCALL_HANDLER(timerfd_settime)(executor, (int)a, (int)b,
                                            (const struct itimerspec *)c,
                                            (struct itimerspec *)d);
  case SYS_utimensat:
    return SYSCALL_HANDLER(utimensat)(executor, (int)a, (const char *)b,
                                      (const struct timespec *)c, (int)d);
  case SYS_wait4:
    return SYSCALL_HANDLER(wait4)(executor, (int)a, (int *)b, (int)c,
                                  (struct rusage *)d);
  // Audited five-argument flat syscalls.
  case SYS_fanotify_mark:
    return SYSCALL_HANDLER(fanotify_mark)(executor, (int)a, (unsigned)b,
                                          (unsigned long long)c, (int)d,
                                          (const char *)e);
  case SYS_fchownat:
    return SYSCALL_HANDLER(fchownat)(executor, (int)a, (const char *)b,
                                     (unsigned)c, (unsigned)d, (int)e);
  case SYS_fsetxattr:
    return SYSCALL_HANDLER(fsetxattr)(executor, (int)a, (const char *)b,
                                      (const void *)c, (unsigned long)d,
                                      (int)e);
  case SYS_linkat:
    return SYSCALL_HANDLER(linkat)(executor, (int)a, (const char *)b, (int)c,
                                   (const char *)d, (int)e);
  case SYS_lsetxattr:
    return SYSCALL_HANDLER(lsetxattr)(executor, (const char *)a,
                                      (const char *)b, (const void *)c,
                                      (unsigned long)d, (int)e);
  case SYS_renameat2:
    return SYSCALL_HANDLER(renameat2)(executor, (int)a, (const char *)b, (int)c,
                                      (const char *)d, (unsigned)e);
  case SYS_setxattr:
    return SYSCALL_HANDLER(setxattr)(executor, (const char *)a, (const char *)b,
                                     (const void *)c, (unsigned long)d, (int)e);
  case SYS_statx:
    return SYSCALL_HANDLER(statx)(executor, (int)a, (const char *)b, (int)c,
                                  (unsigned)d, (struct statx *)e);
  case SYS_name_to_handle_at:
    return SYSCALL_HANDLER(name_to_handle_at)(executor, (int)a, (const char *)b,
                                              (__sanitizer_file_handle *)c,
                                              (int *)d, (int)e);
  // Socket-address and paired-offset syscalls.
  case SYS_bind:
    return SYSCALL_HANDLER(bind)(executor, (int)a, (const struct sockaddr *)b,
                                 (unsigned)c);
  case SYS_copy_file_range:
    return SYSCALL_HANDLER(copy_file_range)(executor, (int)a, (long long *)b,
                                            (int)c, (long long *)d,
                                            (unsigned long)e, (unsigned)f);
  case SYS_getpeername:
    return SYSCALL_HANDLER(getpeername)(executor, (int)a, (struct sockaddr *)b,
                                        (unsigned *)c);
  case SYS_getsockname:
    return SYSCALL_HANDLER(getsockname)(executor, (int)a, (struct sockaddr *)b,
                                        (unsigned *)c);
  case SYS_socketpair:
    return SYSCALL_HANDLER(socketpair)(executor, (int)a, (int)b, (int)c,
                                       (int *)d);
  case SYS_getsockopt:
    return SYSCALL_HANDLER(getsockopt)(executor, (int)a, (int)b, (int)c,
                                       (void *)d, (socklen_t *)e);
  case SYS_setsockopt:
    return SYSCALL_HANDLER(setsockopt)(executor, (int)a, (int)b, (int)c,
                                       (const void *)d, (socklen_t)e);
  case SYS_splice:
    return SYSCALL_HANDLER(splice)(executor, (int)a, (long long *)b, (int)c,
                                   (long long *)d, (unsigned long)e,
                                   (unsigned)f);
  // Cancellable, IPC, descriptor-set, and socket syscalls.
  case SYS_nanosleep:
    return SYSCALL_HANDLER(nanosleep)(executor, (const struct timespec *)a,
                                      (struct timespec *)b);
  case SYS_rt_sigsuspend:
    return SYSCALL_HANDLER(rt_sigsuspend)(executor, (const void *)a,
                                          (unsigned long)b);
  case SYS_getrandom:
    return SYSCALL_HANDLER(getrandom)(executor, (void *)a, (unsigned long)b,
                                      (unsigned)c);
  case SYS_clock_nanosleep:
    return SYSCALL_HANDLER(clock_nanosleep)(executor, (int)a, (int)b,
                                            (const struct timespec *)c,
                                            (struct timespec *)d);
  case SYS_msgsnd:
    return SYSCALL_HANDLER(msgsnd)(executor, (int)a, (const void *)b,
                                   (unsigned long)c, (int)d);
  case SYS_pread64:
    return SYSCALL_HANDLER(pread64)(executor, (int)a, (void *)b,
                                    (unsigned long)c, (long)d);
  case SYS_pwrite64:
    return SYSCALL_HANDLER(pwrite64)(executor, (int)a, (const void *)b,
                                     (unsigned long)c, (long)d);
  case SYS_mq_timedreceive:
    return SYSCALL_HANDLER(mq_timedreceive)(executor, (int)a, (void *)b,
                                            (unsigned long)c, (unsigned *)d,
                                            (const struct timespec *)e);
  case SYS_mq_timedsend:
    return SYSCALL_HANDLER(mq_timedsend)(executor, (int)a, (const void *)b,
                                         (unsigned long)c, (unsigned)d,
                                         (const struct timespec *)e);
  case SYS_msgrcv:
    return SYSCALL_HANDLER(msgrcv)(executor, (int)a, (void *)b,
                                   (unsigned long)c, (long)d, (int)e);
  case SYS_ppoll:
    return SYSCALL_HANDLER(ppoll)(executor, (struct pollfd *)a,
                                  (unsigned long)b, (struct timespec *)c,
                                  (const void *)d, (unsigned long)e);
#ifdef SYS_select
  case SYS_select:
    return SYSCALL_HANDLER(select)(executor, (int)a, (void *)b, (void *)c,
                                   (void *)d, (struct timeval *)e);
#endif
  case SYS_waitid:
    return SYSCALL_HANDLER(waitid)(executor, (int)a, (int)b, (void *)c, (int)d,
                                   (struct rusage *)e);
  case SYS_accept:
    return SYSCALL_HANDLER(accept)(executor, (int)a, (struct sockaddr *)b,
                                   (unsigned *)c);
  case SYS_accept4:
    return SYSCALL_HANDLER(accept4)(executor, (int)a, (struct sockaddr *)b,
                                    (unsigned *)c, (int)d);
  case SYS_connect:
    return SYSCALL_HANDLER(connect)(executor, (int)a,
                                    (const struct sockaddr *)b, (unsigned)c);
  case SYS_recvfrom:
    return SYSCALL_HANDLER(recvfrom)(executor, (int)a, (void *)b,
                                     (unsigned long)c, (int)d,
                                     (struct sockaddr *)e, (unsigned *)f);
  case SYS_sendto:
    return SYSCALL_HANDLER(sendto)(executor, (int)a, (const void *)b,
                                   (unsigned long)c, (int)d,
                                   (const struct sockaddr *)e, (unsigned)f);
  // Signal and timer syscalls.
  case SYS_rt_sigaction:
    return SYSCALL_HANDLER(rt_sigaction)(
        executor, (int)a, (const __sanitizer_kernel_sigaction_t *)b,
        (__sanitizer_kernel_sigaction_t *)c, (unsigned long)d);
  case SYS_rt_sigqueueinfo:
    return SYSCALL_HANDLER(rt_sigqueueinfo)(executor, (pid_t)a, (int)b,
                                            (const siginfo_t *)c);
  case SYS_rt_sigtimedwait:
    return SYSCALL_HANDLER(rt_sigtimedwait)(
        executor, (const sigset_t *)a, (siginfo_t *)b,
        (const struct timespec *)c, (unsigned long)d);
  case SYS_timer_create:
    return SYSCALL_HANDLER(timer_create)(executor, (clockid_t)a,
                                         (const void *)b, (int *)c);
  case SYS_mq_notify:
    return SYSCALL_HANDLER(mq_notify)(executor, (mqd_t)a,
                                      (const struct sigevent *)b);
  // Nested pointer graphs.
  case SYS_epoll_ctl:
    return SYSCALL_HANDLER(epoll_ctl)(executor, (int)a, (int)b, (int)c,
                                      (struct epoll_event *)d);
#ifdef SYS_epoll_wait
  case SYS_epoll_wait:
    return SYSCALL_HANDLER(epoll_wait)(executor, (int)a,
                                       (struct epoll_event *)b, (int)c, (int)d);
#endif
  case SYS_epoll_pwait:
    return SYSCALL_HANDLER(epoll_pwait)(executor, (int)a,
                                        (struct epoll_event *)b, (int)c, (int)d,
                                        (const void *)e, (unsigned long)f);
  case SYS_pselect6:
    return SYSCALL_HANDLER(pselect6)(executor, (int)a, (void *)b, (void *)c,
                                     (void *)d, (struct timespec *)e,
                                     (const sptr *)f);
  case SYS_execve:
    return SYSCALL_HANDLER(execve)(executor, (const char *)a, (char *const *)b,
                                   (char *const *)c);
  case SYS_execveat:
    return SYSCALL_HANDLER(execveat)(executor, (int)a, (const char *)b,
                                     (char *const *)c, (char *const *)d,
                                     (int)e);
  case SYS_readv:
    return SYSCALL_HANDLER(readv)(executor, (int)a,
                                  (const __sanitizer_iovec *)b, (int)c);
  case SYS_writev:
    return SYSCALL_HANDLER(writev)(executor, (int)a,
                                   (const __sanitizer_iovec *)b, (int)c);
  case SYS_preadv:
    return SYSCALL_HANDLER(preadv)(executor, (int)a,
                                   (const __sanitizer_iovec *)b, (int)c,
                                   (long)d, (long)e);
  case SYS_pwritev:
    return SYSCALL_HANDLER(pwritev)(executor, (int)a,
                                    (const __sanitizer_iovec *)b, (int)c,
                                    (long)d, (long)e);
  case SYS_preadv2:
    return SYSCALL_HANDLER(preadv2)(executor, (int)a,
                                    (const __sanitizer_iovec *)b, (int)c,
                                    (long)d, (long)e, (int)f);
  case SYS_pwritev2:
    return SYSCALL_HANDLER(pwritev2)(executor, (int)a,
                                     (const __sanitizer_iovec *)b, (int)c,
                                     (long)d, (long)e, (int)f);
  case SYS_vmsplice:
    return SYSCALL_HANDLER(vmsplice)(executor, (int)a,
                                     (const __sanitizer_iovec *)b,
                                     (unsigned long)c, (unsigned)d);
  case SYS_process_vm_readv:
    return SYSCALL_HANDLER(process_vm_readv)(
        executor, (int)a, (const __sanitizer_iovec *)b, (unsigned long)c,
        (const __sanitizer_iovec *)d, (unsigned long)e, (unsigned long)f);
  case SYS_process_vm_writev:
    return SYSCALL_HANDLER(process_vm_writev)(
        executor, (int)a, (const __sanitizer_iovec *)b, (unsigned long)c,
        (const __sanitizer_iovec *)d, (unsigned long)e, (unsigned long)f);
  case SYS_sendmsg:
    return SYSCALL_HANDLER(sendmsg)(executor, (int)a,
                                    (const __sanitizer_msghdr *)b, (int)c);
  case SYS_recvmsg:
    return SYSCALL_HANDLER(recvmsg)(executor, (int)a, (__sanitizer_msghdr *)b,
                                    (int)c);
  case SYS_recvmmsg:
    return SYSCALL_HANDLER(recvmmsg)(executor, (int)a, (__sanitizer_mmsghdr *)b,
                                     (unsigned long)c, (unsigned)d,
                                     (struct timespec *)e);
  // Virtual-memory ranges without allocation-lifecycle changes.
  case SYS_madvise:
    return SYSCALL_HANDLER(madvise)(executor, (void *)a, (size_t)b, (int)c);
  case SYS_mlock:
    return SYSCALL_HANDLER(mlock)(executor, (const void *)a, (size_t)b);
  case SYS_mlock2:
    return SYSCALL_HANDLER(mlock2)(executor, (const void *)a, (size_t)b,
                                   (unsigned)c);
  case SYS_mprotect:
    return SYSCALL_HANDLER(mprotect)(executor, (void *)a, (size_t)b, (int)c);
  case SYS_pkey_mprotect:
    return SYSCALL_HANDLER(pkey_mprotect)(executor, (void *)a, (size_t)b,
                                          (int)c, (int)d);
  case SYS_msync:
    return SYSCALL_HANDLER(msync)(executor, (void *)a, (size_t)b, (int)c);
  case SYS_munlock:
    return SYSCALL_HANDLER(munlock)(executor, (const void *)a, (size_t)b);
  case SYS_remap_file_pages:
    return SYSCALL_HANDLER(remap_file_pages)(executor, (void *)a, (size_t)b,
                                             (int)c, (size_t)d, (int)e);
  // Unsupported process lifecycle operations.
  case SYS_clone:
    return SYSCALL_HANDLER(clone)(executor, a, b, c, d, e, f);
#ifdef SYS_fork
  case SYS_fork:
    return SYSCALL_HANDLER(fork)(executor);
#endif
#ifdef SYS_vfork
  case SYS_vfork:
    return SYSCALL_HANDLER(vfork)(executor);
#endif
  case SYS_sigaltstack:
    return SYSCALL_HANDLER(sigaltstack)(executor);
  case SYS_set_robust_list:
    return SYSCALL_HANDLER(set_robust_list)(executor);
  // Unsupported command-dependent operations.
  case SYS_semctl:
    return SYSCALL_HANDLER(semctl)(executor);
  case SYS_quotactl:
    return SYSCALL_HANDLER(quotactl)(executor);
  case SYS_mount:
    return SYSCALL_HANDLER(mount)(executor);
  case SYS_ptrace:
    return SYSCALL_HANDLER(ptrace)(executor);
  case SYS_prctl:
    return SYSCALL_HANDLER(prctl)(executor, (int)a, b, c, d, e);
  // Architecture and thread address boundaries.
#ifdef SYS_arch_prctl
  case SYS_arch_prctl:
    return SYSCALL_HANDLER(arch_prctl)(executor, (int)a, (unsigned long)b);
#endif
  case SYS_futex:
    return SYSCALL_HANDLER(futex)(executor, (int *)a, (int)b, (int)c, d,
                                  (int *)e, (int)f);
  case SYS_set_tid_address:
    return SYSCALL_HANDLER(set_tid_address)(executor, (int *)a);
  // Finite command-dependent controls.
  case SYS_fcntl:
    return SYSCALL_HANDLER(fcntl)(executor, (int)a, (int)b, (unsigned long)c);
  case SYS_ioctl:
    return SYSCALL_HANDLER(ioctl)(executor, (int)a, (unsigned)b,
                                  (unsigned long)c);
  case SYS_syslog:
    return SYSCALL_HANDLER(syslog)(executor, (int)a, (char *)b, (int)c);
  case SYS_msgctl:
    return SYSCALL_HANDLER(msgctl)(executor, (int)a, (int)b,
                                   (struct msqid_ds *)c);
  case SYS_shmctl:
    return SYSCALL_HANDLER(shmctl)(executor, (int)a, (int)b,
                                   (struct shmid_ds *)c);
  // Memory-mapping lifecycle metadata.
  case SYS_brk:
    return SYSCALL_HANDLER(brk)(executor, (void *)a);
  case SYS_mmap:
    return SYSCALL_HANDLER(mmap)(executor, (void *)a, (unsigned long)b, (int)c,
                                 (int)d, (int)e, (long)f);
  case SYS_munmap:
    return SYSCALL_HANDLER(munmap)(executor, (void *)a, (uptr)b);
  case SYS_mremap:
    return SYSCALL_HANDLER(mremap)(executor, (void *)a, (uptr)b, (uptr)c,
                                   (int)d, (void *)e);
  case SYS_shmat:
    return SYSCALL_HANDLER(shmat)(executor, (int)a, (const void *)b, (int)c);
  case SYS_shmdt:
    return SYSCALL_HANDLER(shmdt)(executor, (const void *)a);
  }
  const sptr result = executor(number, a, b, c, d, e, f);
  return result;
}

#undef SYSCALL_HANDLER

using RawSyscallFunction = sptr (*)(sptr, sptr, sptr, sptr, sptr, sptr, sptr);

struct RawSyscallExecutor final : SyscallExecutor {
  explicit RawSyscallExecutor(RawSyscallFunction call) : call(call) {}

  sptr operator()(sptr number, sptr a, sptr b, sptr c, sptr d, sptr e,
                  sptr f) const override {
    return call(number, a, b, c, d, e, f);
  }

private:
  RawSyscallFunction call;
};

extern "C" SANITIZER_INTERFACE_ATTRIBUTE sptr
ptsan_musl_syscall(RawSyscallFunction call, sptr number, sptr a, sptr b,
                   sptr c, sptr d, sptr e, sptr f) {
  RawSyscallExecutor executor(call);
  return dispatchSyscall(executor, number, a, b, c, d, e, f);
}

// Do not pull cancellation support into programs using musl's weak fallback.
// When cancellation support is linked, its strong references to __cp_begin,
// __cp_end and __cp_cancel pull in the assembly object and resolve this symbol.
extern "C" SANITIZER_WEAK_ATTRIBUTE sptr
__real___syscall_cp_asm(volatile void *, sptr, sptr, sptr, sptr, sptr, sptr,
                        sptr);

struct CancellableSyscallExecutor final : SyscallExecutor {
  explicit CancellableSyscallExecutor(volatile void *cancel) : cancel(cancel) {}

  sptr operator()(sptr number, sptr a, sptr b, sptr c, sptr d, sptr e,
                  sptr f) const override {
    CHECK(__real___syscall_cp_asm);
    return __real___syscall_cp_asm(cancel, number, a, b, c, d, e, f);
  }

private:
  volatile void *cancel;
};

extern "C" SANITIZER_INTERFACE_ATTRIBUTE sptr
__wrap___syscall_cp_asm(volatile void *cancel, sptr number, sptr a, sptr b,
                        sptr c, sptr d, sptr e, sptr f) {
  // This is musl's internal cancellation flag, not a syscall buffer. Leave
  // cancellation itself to the original assembly, including its PC labels.
  CancellableSyscallExecutor executor(clear_id(cancel));
  return dispatchSyscall(executor, number, a, b, c, d, e, f);
}
