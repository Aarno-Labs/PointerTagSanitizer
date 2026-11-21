//===-- ptsan_thread.h ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of PointerTagSanitizer.
//
// PTSan thread tracking interface.
//
//===----------------------------------------------------------------------===//
// PTSan threads. Based off of MSan Threads
#pragma once

#include "ptsan.h"
#include "ptsan_bounds.h"
#include "ptsan_id_cache.h"
#include "ptsan_random.h"
#include "sanitizer_common/sanitizer_common.h"
#include "sanitizer_common/sanitizer_posix.h"
namespace __ptsan {

extern bool multithreaded;

// Stable ownership storage for all runtime-allocated stack IDs. Zero entries
// are reserved but unpublished, or already removed by an interrupted cleanup.
// A fixed, zero-filled mapping is installed before the thread is published.
// Its configured budget counts outstanding slots, not lifetime allocations.
struct StackIDLog {
  // Compiler ABI: frame/setjmp checkpoints load the first word directly from
  // the TLS object. Keep this layout in sync with PointerTagSanitizer.cpp.
  uptr Cursor;
  PTSanID *IDs;

  void initialize() {
    IDs = (PTSanID *)MmapNoReserveOrDie(
        (uptr)ClStackIDLogCapacity * sizeof(PTSanID), "PTSan stack ID log");
  }

  void release() {
    UnmapOrDie(IDs, (uptr)ClStackIDLogCapacity * sizeof(PTSanID));
  }

  uptr size() const { return Cursor; }

  void push(PTSanID id) {
    uptr index = size();
    if (UNLIKELY(index >= ClStackIDLogCapacity)) {
      Printf("PTSan: per-thread stack ID log exhausted (%u entries).\n"
             "Rebuild with -mllvm -ptsan-stack-id-log-capacity=<larger value> "
             "and rerun.\n", ClStackIDLogCapacity);
      internal__exit(1);
    }
    // Reserve before publishing so a nested handler uses a different slot.
    // Nonlocal cleanup can skip the zero if publication was abandoned. Signal
    // contexts are nested on one thread, so naturally atomic plain accesses
    // suffice; only compiler reordering across this boundary must be prevented.
    Cursor = index + 1;
    atomic_signal_fence(memory_order_seq_cst);
    IDs[index] = id;
  }

  PTSanID back() const {
    CHECK_GT(size(), 0U);
    return IDs[size() - 1];
  }

  PTSanID pop() {
    uptr end = size();
    CHECK_GT(end, 0U);
    // Clear ownership before lowering the cursor: a handler can then reuse
    // the slot without the interrupted pop touching its contents afterward.
    PTSanID id = IDs[end - 1];
    IDs[end - 1] = 0;
    atomic_signal_fence(memory_order_seq_cst);
    Cursor = end - 1;
    return id;
  }
};

// This one is important; without it, the generated IR will load the wrong value
static_assert(__builtin_offsetof(StackIDLog, Cursor) == 0);
static_assert(__builtin_offsetof(StackIDLog, IDs) == sizeof(uptr));

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
THREADLOCAL StackIDLog ptsan_stack_id_log;

class PTSanThread {
 public:
  static PTSanThread *Create(thread_callback_t start_routine, void *arg);
  static void TSDDtor(void *tsd);
  void Destroy();
  // Free the record and return its bucket after teardown or failed creation.
  // Destroy has already released cached IDs; an unstarted thread has none.
  // This does not touch the calling thread's TLS.
  void Release();

  static void InitTLS();
  thread_return_t ThreadStart();
  int ThreadStartC11();

  PTSanID takeID() {
#ifdef PTSAN_MUSL_INTERCEPTION
    if (ptsan_in_signal_context) {
      return SignalIdCache.take();
    }
#endif
    return IdCache.take();
  }

  void returnID(PTSanID id) {
#ifdef PTSAN_MUSL_INTERCEPTION
    if (ptsan_in_signal_context) {
      SignalIdCache.returnID(id);
      return;
    }
#endif
    IdCache.returnID(id);
  }

  __sanitizer_sigset_t starting_sigset_;
  int destructor_iterations_;

  StackIDBucket *stack_id_bucket;
  u64 next_stack_id;
  u64 stack_id_limit;

  IDCache IdCache;
  RandomStream Random;
  // Kernel-facing argv/envp arrays survive a successful exec in a child
  // created with CLONE_VM. Keep them for reuse by the suspended parent.
  char **ExecArgvScratch;
  uptr ExecArgvCapacity;
  char **ExecEnvpScratch;
  uptr ExecEnvpCapacity;
#ifdef PTSAN_32_BIT_IDS
  // A frame consumes one nonzero, 4 KiB-aligned salt inline. Refills use
  // the same Random stream as heap origins; wrapped handlers bypass this cache.
  static constexpr u32 StackOriginCount = 1024;
  // The final word stays zero. It triggers refill without a second TLS load,
  // and is never consumed by generated code, which requires a nonzero salt.
  u32 StackOrigins[StackOriginCount + 1];
#endif
#ifdef PTSAN_MUSL_INTERCEPTION
  SignalSafeIDCache SignalIdCache;
#endif

 private:
  // NOTE: There is no PTSanThread constructor. It is allocated
  // via mmap() and *must* be valid in zero-initialized state.
  thread_callback_t start_routine_;
  void *arg_;
};

PTSanThread *GetCurrentThread();
void SetCurrentThread(PTSanThread *t);
// Final teardown only, with signals blocked and cached IDs already released.
void ClearCurrentThread();

#ifdef PTSAN_MUSL_INTERCEPTION
// Call only at a final musl exit boundary; leaves all signals blocked.
void FinishMuslThread();
#endif

} // namespace __ptsan
