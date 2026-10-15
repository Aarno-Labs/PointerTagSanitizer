//===-- ptsan_interceptors.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of PointerTagSanitizer.
//
// PTSan libc interceptor declarations.
//
//===----------------------------------------------------------------------===//

#pragma once
#include "sanitizer_common/sanitizer_internal_defs.h"
#include "sanitizer_common/sanitizer_common.h"
#include <stddef.h>  // size_t (freestanding-safe)

extern SANITIZER_INTERFACE_ATTRIBUTE THREADLOCAL u32 ptsan_in_libc;

// Scope guard for ptsan_in_libc: guarantees the decrement on every exit path.
class InLibcGuard {
public:
  __attribute__((always_inline)) InLibcGuard()  { ++ptsan_in_libc; }
  __attribute__((always_inline)) ~InLibcGuard() { --ptsan_in_libc; }
  InLibcGuard(const InLibcGuard&) = delete;
  InLibcGuard& operator=(const InLibcGuard&) = delete;
};

// PTSAN_ENV_GUARD() swaps in an untagged environ for the enclosing scope: libc
// functions reading environ internally (getopt*, exec*, getaddrinfo, popen) fault on a tag.
extern "C" char **environ;  // C linkage to match <unistd.h>
extern __sanitizer::Mutex ptsan_env_lock;
size_t ptsan_strv_len(char *const *v_in);
void ptsan_strv_deep_untag_into(char *const *src_in, char **dst, size_t n);

// RAII environ swap: restores environ and unlocks on every exit path. The clean
// buffer must be alloca'd in the caller's frame, so go through PTSAN_ENV_GUARD().
class PtsanEnvGuard {
  __sanitizer::Lock lock_;  // GenericScopedLock<Mutex>: locks here, unlocks in dtor
  char **saved_;
public:
  PtsanEnvGuard(char **clean, size_t envc)
      : lock_(&ptsan_env_lock), saved_(environ) {
    ptsan_strv_deep_untag_into(saved_, clean, envc);
    environ = clean;
  }
  ~PtsanEnvGuard() { environ = saved_; }
};

#define PTSAN_ENV_GUARD()                                                       \
  ENSURE_PTSAN_INITED();                                                        \
  InLibcGuard __ptsan_in_libc;                                                  \
  size_t __ptsan_envc = ptsan_strv_len(environ);                               \
  char **__ptsan_clean_envp = (char **)__builtin_alloca(                       \
      (__ptsan_envc + 1) * sizeof(char *));                                    \
  PtsanEnvGuard __ptsan_env_guard(__ptsan_clean_envp, __ptsan_envc);

namespace __ptsan {
  void InitializeInterceptors();
  void InitializeLibCHeaderInterceptors();
  void InitializeFormatInterceptors();
} // namespace __ptsan
