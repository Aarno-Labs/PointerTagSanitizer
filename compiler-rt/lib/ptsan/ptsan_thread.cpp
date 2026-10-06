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
#include "sanitizer_common/sanitizer_tls_get_addr.h"
#ifdef PTSAN_MUSL_INTERCEPTION
#include "ptsan_musl_internal.h"
#endif

namespace __ptsan {

volatile u64 num_threads = 0;
bool multithreaded = false;

struct TLSCallbacks {
  void (*externs)();
  void (*tags)();
  void (*destroy)();
};

// Zero-initialized storage, with no dynamic constructor that could reset early
// registrations. Module constructors register before application threads start.
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
void ptsan_initialize_tls(u16 *id, void *base, u64 size) {
  // Coalesced definitions can initialize the same ID slot more than once.
  if (*id != 0) {
    return;
  }

  *id = ptsan_get_next_nonstack_id(base, size, FREE_TY::NONE);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE
void ptsan_destroy_tls(u16 *id, void *base) {
  if (*id == 0) {
    return;
  }

  ptsan_mark_ptr_freed(set_id(base, *id), FREE_TY::NONE);
  // Clear the ID so another module sharing this slot won't retire it twice.
  *id = 0;
}

static int InitStackIDBucket() {
  for (u32 i = 0; i < ptsan_active_stack_id_buckets; i++) {
    StackIDBucket &B = StackIDBuckets[i];
    // Claim exclusively and acquire the previous owner's cleanup.
    if (__atomic_exchange_n(&B.avaliable, false, __ATOMIC_ACQUIRE)) {
      return (int)i;
    }
  }

  // No more buckets :(
  return -1;
}

PTSanThread *PTSanThread::Create(thread_callback_t start_routine,
                                 void *arg) {
  u64 old_num_threads = __atomic_fetch_add(&num_threads, 1, __ATOMIC_RELAXED);
  if (old_num_threads > 0) {
    multithreaded = true;
  }

  uptr PageSize = GetPageSizeCached();
  uptr size = RoundUpTo(sizeof(PTSanThread), PageSize);
  PTSanThread *thread = (PTSanThread*)MmapOrDie(size, __func__);
  thread->start_routine_ = start_routine;
  thread->arg_ = arg;
  thread->destructor_iterations_ = GetPthreadDestructorIterations();
  thread->stack_id_bucket_no = InitStackIDBucket();

  return thread;
}

void PTSanThread::SetThreadStack() {
#ifdef PTSAN_MUSL_INTERCEPTION
  if (!IsMainThread()) {
    // Avoid passing a raw runtime-local pthread_attr_t to instrumented musl.
    void *top;
    uptr size;
    ptsan_musl_thread_stack(&top, &size);
    stack_.top = get_addr(top);
    stack_.bottom = stack_.top - size;
  } else
#endif
  {
    GetThreadStackTopAndBottom(IsMainThread(), &stack_.top, &stack_.bottom);
  }
  int local;
  CHECK(AddrIsInStack((uptr)&local));
}

void PTSanThread::Init() {
  SetThreadStack();
}

void PTSanThread::TSDDtor(void *tsd) {
  PTSanThread *t = (PTSanThread*)tsd;
  t->Destroy();
}

void PTSanThread::Destroy() {
  // The glibc final-iteration TSD hook still has an ordering hole for user
  // destructors that re-register themselves through that same iteration.
  for (const auto &module : TLSInitializers) {
    module.destroy();
  }
  // Nonlocal exits can leave frame IDs live until thread teardown.
  RestoreStackIDCheckpoint(*this, {});
  slow_stack_ids.Destroy();
  repeatable_alloca_ids.Destroy();
  Release();
  DTLS_Destroy();
}

void PTSanThread::Release() {
  __atomic_fetch_sub(&num_threads, 1, __ATOMIC_RELAXED);

  if (stack_id_bucket_no >= 0) {
    __atomic_store_n(&StackIDBuckets[stack_id_bucket_no].avaliable, true,
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

PTSanThread::StackBounds PTSanThread::GetStackBounds() const {
  return {stack_.bottom, stack_.top};
}

uptr PTSanThread::stack_top() { return GetStackBounds().top; }

uptr PTSanThread::stack_bottom() { return GetStackBounds().bottom; }

bool PTSanThread::AddrIsInStack(uptr addr) {
  const auto bounds = GetStackBounds();
  // Stack membership depends on addresses, not the IDs carried by pointers.
  return get_addr(addr) >= get_addr(bounds.bottom) &&
         get_addr(addr) < get_addr(bounds.top);
}

} // namespace __ptsan
