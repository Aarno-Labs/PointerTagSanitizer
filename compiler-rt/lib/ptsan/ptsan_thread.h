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
#include "sanitizer_common/sanitizer_common.h"
#include "sanitizer_common/sanitizer_posix.h"
namespace __ptsan {

extern bool multithreaded;

class PTSanThread {
 public:
  static PTSanThread *Create(thread_callback_t start_routine, void *arg);
  static void TSDDtor(void *tsd);
  void Destroy();

  void Init();  // Should be called from the thread itself.
  thread_return_t ThreadStart();

  uptr stack_top();
  uptr stack_bottom();
  bool IsMainThread() { return start_routine_ == nullptr; }

  bool AddrIsInStack(uptr addr);

  __sanitizer_sigset_t starting_sigset_;
  int destructor_iterations_;

  int stack_id_bucket_no;
  u16 next_stack_id;
  u16 stack_id_limit;

 private:
  // NOTE: There is no PTSanThread constructor. It is allocated
  // via mmap() and *must* be valid in zero-initialized state.
  void SetThreadStack();
  struct StackBounds {
    uptr bottom;
    uptr top;
  };
  StackBounds GetStackBounds() const;
  thread_callback_t start_routine_;
  void *arg_;

  StackBounds stack_;
};

PTSanThread *GetCurrentThread();
void SetCurrentThread(PTSanThread *t);

} // namespace __ptsan
