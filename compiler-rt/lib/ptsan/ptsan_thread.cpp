//===-- ptsan_thread.cpp --------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of PointerTagSanitizer.
//
// PTSan thread tracking.
//
//===----------------------------------------------------------------------===//

#include "ptsan_thread.h"

#include "ptsan.h"
#include "ptsan_constants.h"
#include "ptsan_bounds.h"
#include "ptsan_ptr_utils.h"
#include "ptsan_signal.h"
#include "sanitizer_common/sanitizer_tls_get_addr.h"

namespace __ptsan {

extern "C" {
SANITIZER_INTERFACE_ATTRIBUTE THREADLOCAL StackIDLog ptsan_stack_id_log;
}

volatile u64 num_threads = 0;
bool multithreaded = false;

struct TLSCallbacks {
  void (*externs)();
  void (*tags)();
  void (*destroy)();
};

// Zero-initialized storage, with no dynamic constructor that could reset early
// registrations. Musl registers before TLS; host libc uses module constructors.
static InternalMmapVectorNoCtor<TLSCallbacks> TLSInitializers;

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_register_tls(void (*externs)(), void (*tags)(), void (*destroy)()) {
  TLSInitializers.push_back({externs, tags, destroy});
}

void PTSanThread::InitTLS() {
  for (const auto &module : TLSInitializers) {
    module.externs();
  }
  for (const auto &module : TLSInitializers) {
    module.tags();
  }
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_initialize_tls(PTSanID *id, void *base, u64 size, u64 alignment) {
  // Coalesced definitions can initialize the same ID slot more than once.
  if (*id != 0) {
    return;
  }

  // Main thread tls initialization can come before our cache machinery
  // is set up; in that case, get ids straight from the pool
  if (!GetCurrentThread()) {
    *id = get_id_direct(base, size, FREE_TY::NONE, alignment);
  } else {
    *id = ptsan_get_next_nonstack_id(base, size, FREE_TY::NONE, alignment);
  }
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_destroy_tls(PTSanID *id, void *base) {
  if (*id == 0) {
    return;
  }

  ptsan_mark_ptr_freed(set_id(base, *id), FREE_TY::NONE);
  // Clear the ID so another module sharing this slot won't retire it twice.
  *id = 0;
}

static StackIDBucket *InitStackIDBucket(RandomStream &Random) {
  if (!ClStackIDThreadBuckets) {
    return nullptr;
  }
  u32 Index = ClRandomizeIDs
                  ? Random.bounded(ClStackIDThreadBuckets)
                  : 0;
  for (u32 Remaining = ClStackIDThreadBuckets; Remaining; --Remaining) {
    StackIDBucket *Bucket = &StackIDBuckets[Index];
    // Claim exclusively and acquire the previous owner's cleanup.
    if (__atomic_exchange_n(&Bucket->avaliable, false, __ATOMIC_ACQUIRE)) {
      return Bucket;
    }
    if (++Index == ClStackIDThreadBuckets) {
      Index = 0;
    }
  }
  return nullptr;
}

PTSanThread *PTSanThread::Create(thread_callback_t start_routine,
                                 void *arg) {
  u64 old_num_threads = __atomic_fetch_add(&num_threads, 1, __ATOMIC_RELAXED);
  if (old_num_threads > 0) {
    if (!multithreaded &&
        MultithreadedIDCacheCapacity != NormalIDCacheCapacity) {
      // Before publishing the second thread, return the sole thread's larger
      // single-thread batches so every cache starts the multithreaded phase at
      // its smaller capacity.
      ScopedSignalBlock block;
      PTSanThread *current = GetCurrentThread();
      CHECK(current);
      current->IdCache.release();
      current->IdCache.setActiveCapacity(MultithreadedIDCacheCapacity);
    }
    multithreaded = true;
  }

  uptr PageSize = GetPageSizeCached();
  uptr size = RoundUpTo(sizeof(PTSanThread), PageSize);
  PTSanThread *thread = (PTSanThread*)MmapOrDie(size, __func__);
  thread->IdCache.initialize(multithreaded ? MultithreadedIDCacheCapacity
                                           : NormalIDCacheCapacity);
  thread->Random.initialize();
  thread->start_routine_ = start_routine;
  thread->arg_ = arg;
  thread->destructor_iterations_ = GetPthreadDestructorIterations();
  PTSanThread *Current = GetCurrentThread();
  thread->stack_id_bucket =
      InitStackIDBucket(Current ? Current->Random : thread->Random);

  return thread;
}

void PTSanThread::TSDDtor(void *tsd) {
  PTSanThread *t = (PTSanThread*)tsd;
  t->Destroy();
}

void PTSanThread::Destroy() {
  // Exit hooks keep signals blocked and this thread current through ID cleanup.
  // The glibc final-iteration TSD hook still has an ordering hole for user
  // destructors that re-register themselves through that same iteration.
  for (const auto &module : TLSInitializers) {
    module.destroy();
  }
  // Nonlocal exits can leave frame IDs live until thread teardown.
  ptsan_free_stack_ids(0);
  // pthread_exit can bypass fast-frame epilogues. Retire their spans before
  // another thread claims this bucket, retaining decode bases and origins.
  if (stack_id_bucket) {
    for (u64 id = stack_id_bucket->start; id < ptsan_next_stack_id; ++id) {
      ptsan_bounds[id].span = 0;
    }
  }
  IdCache.release();
  {
    ScopedSignalBlock block;
    if (ExecArgvScratch) {
      InternalFree(ExecArgvScratch);
    }
    if (ExecEnvpScratch) {
      InternalFree(ExecEnvpScratch);
    }
  }
#ifdef PTSAN_MUSL_INTERCEPTION
  SignalIdCache.release();
#endif
  ClearCurrentThread();
  ptsan_stack_id_log.release();
  Release();
  DTLS_Destroy();
}

void PTSanThread::Release() {
  __atomic_fetch_sub(&num_threads, 1, __ATOMIC_RELAXED);

  if (stack_id_bucket) {
    __atomic_store_n(&stack_id_bucket->avaliable, true,
                     __ATOMIC_RELEASE);
  }

  uptr size = RoundUpTo(sizeof(PTSanThread), GetPageSizeCached());
  UnmapOrDie(this, size);
}

thread_return_t PTSanThread::ThreadStart() {
  if (!start_routine_) {
    // start_routine_ == 0 if we're on the main thread or on one of the
    // OS X libdispatch worker threads. But nobody is supposed to call
    // ThreadStart() for the worker threads.
    return 0;
  }

  thread_return_t res = start_routine_(arg_);

  return res;
}

int PTSanThread::ThreadStartC11() {
  // musl passes C11 callbacks through __pthread_create's pthread signature.
  return reinterpret_cast<int (*)(void *)>(start_routine_)(arg_);
}

} // namespace __ptsan
