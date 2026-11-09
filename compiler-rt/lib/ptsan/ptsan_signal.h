//===-- ptsan_signal.h - Signal-safe transactions -----------------*- C++ -*-===//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef PTSAN_SIGNAL_H
#define PTSAN_SIGNAL_H

#include "sanitizer_common/sanitizer_allocator_internal.h"
#include "sanitizer_common/sanitizer_linux.h"
#include "sanitizer_common/sanitizer_platform_limits_posix.h"
#include <signal.h>

namespace __ptsan {

#ifdef PTSAN_MUSL_INTERCEPTION
// Construction saves the calling thread's current signal mask and immediately
// blocks all blockable signals. Destruction restores that thread's saved mask.
// This includes musl's cancellation signal and asynchronously sent instances of
// SIGSEGV, SIGBUS, etc. Protected transactions must not touch faultable
// application buffers or call user code.
class ScopedSignalBlock {
  __sanitizer::__sanitizer_sigset_t Saved;

public:
  ScopedSignalBlock() {
    __sanitizer::__sanitizer_sigset_t All;
    __sanitizer::internal_sigfillset(&All);
    // Install All as the new mask and write the previous mask into Saved.
    __sanitizer::SetSigProcMask(&All, &Saved);
  }
  ~ScopedSignalBlock() { __sanitizer::SetSigProcMask(&Saved, nullptr); }
  // Copy the pre-blocking mask; this does not change any thread's signal mask.
  void copySavedMask(__sanitizer::__sanitizer_sigset_t *Destination) const {
    // The raw syscall initializes only the kernel-sized part of sigset_t.
    __sanitizer::internal_memcpy(
        Destination, &Saved, sizeof(__sanitizer::__sanitizer_kernel_sigset_t));
  }
  ScopedSignalBlock(const ScopedSignalBlock &) = delete;
  ScopedSignalBlock &operator=(const ScopedSignalBlock &) = delete;
};
#else
// The host-libc runtime does not wrap application signal handlers, so it does
// not offer signal-reentrant runtime operations. Keep shared call sites simple:
// their critical sections still take the cross-thread lock where required, but
// do not pay for signal-mask syscalls in this configuration.
class [[maybe_unused]] ScopedSignalBlock {
public:
  ScopedSignalBlock() = default;
  ScopedSignalBlock(const ScopedSignalBlock &) = delete;
  ScopedSignalBlock &operator=(const ScopedSignalBlock &) = delete;
};
#endif

// Syscall shadows use InternalAlloc's shared lock.
// Mask only the allocator operation, never a potentially blocking syscall.
inline void *SignalSafeInternalAlloc(__sanitizer::uptr Size) {
  ScopedSignalBlock Block;
  return __sanitizer::InternalAlloc(Size);
}

inline void SignalSafeInternalFree(void *Pointer) {
  ScopedSignalBlock Block;
  __sanitizer::InternalFree(Pointer);
}

// Static-musl entries emitted into the ptsan_signal_entries linker section.
// The binding never changes after loading the executable.
struct SignalEntry {
  void *Handler;
  void *Entry;
};
static_assert(sizeof(SignalEntry) == 16);

// Abort if the handler has no compatible compiler-generated entry.
const SignalEntry *GetSignalEntry(void *Handler);
// Preserve SIG_DFL/SIG_IGN; abort for an unknown handler entry.
void *OriginalSignalHandler(void *Entry);

} // namespace __ptsan

#endif
