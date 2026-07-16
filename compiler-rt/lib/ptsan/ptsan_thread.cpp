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
#include "sanitizer_common/sanitizer_tls_get_addr.h"

namespace __ptsan {

volatile u64 num_threads = 0;
bool multithreaded = false;

static int InitStackIDBucket() {
  for (u32 i = 0; i < ptsan_active_stack_id_buckets; i++) {
    StackIDBucket &B = StackIDBuckets[i];
    if (B.avaliable) {
      B.avaliable = false;
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
  GetThreadStackTopAndBottom(IsMainThread(), &stack_.top, &stack_.bottom);
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
  __atomic_fetch_sub(&num_threads, 1, __ATOMIC_RELAXED);

  if (stack_id_bucket_no >= 0) {
    StackIDBuckets[stack_id_bucket_no].avaliable = true;
  }

  uptr size = RoundUpTo(sizeof(PTSanThread), GetPageSizeCached());
  UnmapOrDie(this, size);
  DTLS_Destroy();
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

PTSanThread::StackBounds PTSanThread::GetStackBounds() const {
  return {stack_.bottom, stack_.top};
}

uptr PTSanThread::stack_top() { return GetStackBounds().top; }

uptr PTSanThread::stack_bottom() { return GetStackBounds().bottom; }

bool PTSanThread::AddrIsInStack(uptr addr) {
  const auto bounds = GetStackBounds();
  return addr >= bounds.bottom && addr < bounds.top;
}

} // namespace __ptsan
