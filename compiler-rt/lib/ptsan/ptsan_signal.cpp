//===-- ptsan_signal.cpp - Static-musl signal entry -------------------------===//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ptsan_signal.h"
#include "ptsan_bounds.h"
#include "ptsan_constants.h"
#include "ptsan_ptr_utils.h"
#include "ptsan_thread.h"
#include <ucontext.h>

using namespace __ptsan;

extern "C" {
extern const SignalEntry
    __start_ptsan_signal_entries[] SANITIZER_WEAK_ATTRIBUTE;
extern const SignalEntry __stop_ptsan_signal_entries[] SANITIZER_WEAK_ATTRIBUTE;
}

const SignalEntry *__ptsan::GetSignalEntry(void *Handler) {
  for (auto *E = __start_ptsan_signal_entries;
       E && E != __stop_ptsan_signal_entries; ++E) {
    if (E->Handler == Handler) {
      return E;
    }
  }
  REPORT_ERROR("PTSan: signal handler has no compiler-generated entry\n");
  Abort();
}

void *__ptsan::OriginalSignalHandler(void *Entry) {
  if (Entry == (void *)SIG_DFL || Entry == (void *)SIG_IGN) {
    return Entry;
  }
  for (auto *E = __start_ptsan_signal_entries;
       E && E != __stop_ptsan_signal_entries; ++E) {
    if (E->Entry == Entry) {
      return E->Handler;
    }
  }
  REPORT_ERROR("PTSan: registered signal handler is not a compiler-generated entry\n");
  Abort();
}

// These helpers and the generated forwarding entries are uninstrumented.
// Clear in_libc so the application handler does not inherit an interrupted
// libc boundary; restore the interrupted value on normal return.
// On a nonlocal exit, cleanup at the instrumented setjmp destination
// restores stack ownership and clears in_libc, bypassing these epilogues.
class SignalContextScope {
  const u32 SavedInLibc;
  const u64 SavedNextStackID;
  const PTSanID SavedPendingStackIDs;
  const u8 SavedInSignalContext;

public:
  SignalContextScope()
      : SavedInLibc(ptsan_in_libc), SavedNextStackID(ptsan_next_stack_id),
        SavedPendingStackIDs(ptsan_pending_stack_ids),
        SavedInSignalContext(ptsan_in_signal_context) {
    ptsan_in_libc = 0;
    ptsan_in_signal_context = 1;
    // Keep the interrupted thread's fast range untouched. The generated fast
    // path will select its ordinary slow-ID fallback while the handler runs.
    ptsan_next_stack_id = ptsan_stack_id_limit;
    ptsan_pending_stack_ids = 0;
  }

  ~SignalContextScope() {
    ptsan_pending_stack_ids = SavedPendingStackIDs;
    ptsan_next_stack_id = SavedNextStackID;
    ptsan_in_signal_context = SavedInSignalContext;
    ptsan_in_libc = SavedInLibc;
  }

  SignalContextScope(const SignalContextScope &) = delete;
  SignalContextScope &operator=(const SignalContextScope &) = delete;
};

extern "C" SANITIZER_INTERFACE_ATTRIBUTE void
ptsan_signal_entry_0(void (*Handler)(void)) {
  SignalContextScope Scope;
  Handler();
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE void
ptsan_signal_entry_1(int Signal, void (*Handler)(int)) {
  SignalContextScope Scope;
  Handler(Signal);
}

extern "C" SANITIZER_INTERFACE_ATTRIBUTE void
ptsan_signal_entry_3(int Signal, siginfo_t *Info, void *Context,
                     void (*Handler)(int, siginfo_t *, void *)) {
  SignalContextScope Scope;
  const uptr Cursor = ptsan_stack_id_log.size();
  PTSanID InfoID = ptsan_get_repeatable_alloca_id(
      Info, sizeof(siginfo_t), alignof(siginfo_t));
  PTSanID ContextID = ptsan_get_repeatable_alloca_id(
      Context, sizeof(ucontext_t), alignof(ucontext_t));
  Handler(Signal, set_id(Info, InfoID), set_id(Context, ContextID));
  ptsan_free_stack_ids(Cursor);
}
