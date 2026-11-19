//===-- ptsan.cpp ---------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of PointerTagSanitizer.
//
// PTSan runtime initialization and shutdown.
//
//===----------------------------------------------------------------------===//

#include "ptsan.h"
#include "interceptors/ptsan_interceptors.h"
#include "ptsan_bounds.h"
#include "ptsan_origins.h"
#include "ptsan_stats.h"
#include "ptsan_thread.h"

#include <pthread.h>
#include <stdint.h>
#include <sys/syscall.h>
#if defined(__x86_64__)
#include <asm/prctl.h>  // x86-only (arch_prctl constants for LAM)
#endif

#include "sanitizer_common/sanitizer_linux.h"
#include "sanitizer_common/sanitizer_file.h"
#include "sanitizer_common/sanitizer_flags.h"

using namespace __ptsan;

bool __ptsan::ptsan_initialized;
bool __ptsan::ptsan_init_is_running;


namespace __ptsan {
#ifndef PTSAN_MUSL_INTERCEPTION
// Thread interception
static pthread_key_t tsd_key;
static bool tsd_key_inited = false;

void PTSanTSDInit(void (*destructor)(void *tsd)) {
  CHECK(!tsd_key_inited);
  tsd_key_inited = true;
  int result = pthread_key_create(&tsd_key, destructor);
  CHECK_EQ(result, 0);
}
#endif

static THREADLOCAL PTSanThread* ptsan_current_thread;

PTSanThread *GetCurrentThread() {
  return ptsan_current_thread;
}

void SetCurrentThread(PTSanThread *t) {
  // Make sure we do not reset the current PTSanThread.
  CHECK_EQ(0, ptsan_current_thread);
  // The child owns its TLS log. Initialize it before publishing the thread
  // record or entering instrumented code; application signals remain blocked.
  ptsan_stack_id_log.initialize();
  ptsan_current_thread = t;
#ifdef PTSAN_32_BIT_IDS
  // Start at the zero sentinel, so the first frame takes the refill path.
  // Signals remain blocked here; a refill later publishes one cursor store.
  ptsan_stack_origin_next = t->StackOrigins + t->StackOriginCount;
#endif

  if (t->stack_id_bucket) {
    ptsan_next_stack_id = t->stack_id_bucket->start;
    ptsan_stack_id_limit = t->stack_id_bucket->end;
  } else {
    // 1 > 0, so we always go to the slow case
    ptsan_next_stack_id = 1;
    ptsan_stack_id_limit = 0;
  }

#ifndef PTSAN_MUSL_INTERCEPTION
  // An uninstrumented libc needs no PTSan state after its TSD destructors.
  CHECK(tsd_key_inited);
  pthread_setspecific(tsd_key, (void *)t);
#endif
}

void ClearCurrentThread() {
#ifdef PTSAN_32_BIT_IDS
  ptsan_stack_origin_next = nullptr;
#endif
  ptsan_current_thread = nullptr;
  // Publish the cleared pointer before the thread record is unmapped.
  atomic_signal_fence(memory_order_seq_cst);
}

#ifdef PTSAN_MUSL_INTERCEPTION
void FinishMuslThread() {
  // Instrumented musl still uses stack and TLS IDs after running TSD
  // destructors. Retire them only at SYS_exit or __unmapself, with no return
  // to instrumented code. Keep every signal blocked through the final exit.
  __sanitizer_sigset_t set;
  internal_sigfillset(&set);
  SetSigProcMask(&set, nullptr);

  PTSanThread *t = ptsan_current_thread;
  // A failed pthread_create can exit before PTSanThreadStartFunc runs.
  if (t) {
    t->Destroy();
  }
}
#else
void PTSanTSDDtor(void *tsd) {
  PTSanThread *t = (PTSanThread*)tsd;
  if (t->destructor_iterations_ > 1) {
    t->destructor_iterations_--;
    int result = pthread_setspecific(tsd_key, tsd);
    CHECK_EQ(result, 0);
    return;
  }
  ScopedBlockSignals block(nullptr);
  PTSanThread::TSDDtor(tsd);
}
#endif
} // namespace __ptsan


void ValidateCls() {
#ifdef PTSAN_STRICT_32_BIT_IDS
  CHECK(ClStrictMode);
#elif defined(PTSAN_32_BIT_IDS)
  CHECK(!ClStrictMode);
#endif
#ifdef PTSAN_32_BIT_IDS
  CHECK(ClAbortOnError);
  CHECK(IsPowerOfTwo(ClID32OriginQuantum));
  CHECK_LE(ClID32OriginQuantum, 1ULL << 31);
  const u64 PageSize = GetPageSizeCached();
  if (ClID32OriginQuantum < PageSize) {
    Printf("PTSan ID32 origin quantum (%u) is smaller than the kernel page "
           "size (%llu)\n", ClID32OriginQuantum, PageSize);
    internal__exit(1);
  }
#endif
  // If you hit these, go change the define in ptsan.h
  #ifdef PTSAN_LAM
    CHECK(ClLAM);
  #else
    CHECK(!ClLAM);
  #endif

  #ifdef PTSAN_VALIDATE
    CHECK(ClValidate);
  #else
    CHECK(!ClValidate);
    CHECK(!ClPrintStats);
  #endif
}

extern "C" {

static void ptsan_atexit() {
// Needs an ifdef since these stats don't exist without it
#ifdef PTSAN_VALIDATE
if (ClPrintStats) {
  Printf("Pointer Tag Sanitizer Exit Stats:\n");
  Printf("Max used IDs at once: %llu (0x%llx)\n",
          __ptsan_max_used_ids, __ptsan_max_used_ids);
  Printf("Max used stack IDs at once: %llu (0x%llx)\n",
          __ptsan_max_used_stack_ids, __ptsan_max_used_stack_ids);
  Printf("Unreclaimed IDs at exit: %llu\n", __ptsan_used_ids - __ptsan_used_global_ids);
  Printf("Global IDs used: %llu\n", __ptsan_used_global_ids);
  Printf("Total allocas called: %llu\n", ptsan_total_allocas);
  Printf("Local allocas called: %llu\n", ptsan_local_allocas);
  Printf("Functions by slow-path alloca amount:\n");
  for (int i = 0; i < 4; i++) {
    Printf("  %d: %llu\n", i + 1, ptsan_n_alloca_fn_calls[i]);
  }
  Printf("  n: %llu\n", ptsan_n_alloca_fn_calls[4]);
  Printf("Checks called:\n");
  u64 total_checks = 0;
  for (int i = 0; i < END_TYPE; i++) {
    total_checks += ptsan_called_checks[i];
    Printf("  %s: %llu\n", ChkTyToString((CHK_TY)i), ptsan_called_checks[i]);
  }
  Printf("Total checks called: %llu\n", total_checks);
}
#endif
}

static inline long ptsan_raw_syscall2(long nr, long a1, long a2) {
  long ret;
#if defined(__x86_64__)
  register long rax __asm__("rax") = nr;
  register long rdi __asm__("rdi") = a1;
  register long rsi __asm__("rsi") = a2;
  __asm__ __volatile__(
      "syscall"
      : "=a"(ret)
      : "a"(rax), "D"(rdi), "S"(rsi)
      : "rcx", "r11", "memory");
#elif defined(__aarch64__)
  register long x8 __asm__("x8") = nr;
  register long x0 __asm__("x0") = a1;
  register long x1 __asm__("x1") = a2;
  __asm__ __volatile__(
      "svc #0"
      : "=r"(x0)
      : "r"(x8), "0"(x0), "r"(x1)
      : "memory");
  ret = x0;
#else
# error "ptsan_raw_syscall2: add arch support"
#endif
  return ret;
}

#define ARCH_ENABLE_TAGGED_ADDR		0x4002
#define ARCH_LAM_U48                    15
int ptsan_enable_lam_u48(void) {
#ifdef SYS_arch_prctl
  // LAM (Linear Address Masking) is x86-64 only.
  long ret = ptsan_raw_syscall2(SYS_arch_prctl,
                                (long)ARCH_ENABLE_TAGGED_ADDR,
                                (long)ARCH_LAM_U48);
  if (ret < 0) {
    return -1;
  }
  return 0;
#else
  return -1;
#endif
}

SANITIZER_INTERFACE_ATTRIBUTE void __ptsan_init_ids() {
  static bool initialized;
  if (initialized) {
    return;
  }
  SetCommonFlagsDefaults();
  ValidateCls();
  InitializeIDs();
  initialized = true;
}

static void ptsanInitStart() {
  ptsan_init_is_running = true;

  __ptsan_init_ids();

  if (ClLAM)
    ptsan_enable_lam_u48();

  //Printf("Starting intialization of ptsan\n");

  InitializeInterceptors();

  // Libc-dependent setup can enter instrumented code before initialization
  // finishes. Give it valid slow-path stack-ID state; SetCurrentThread installs
  // the normal per-thread bucket below.
  ptsan_next_stack_id = 1;
  ptsan_stack_id_limit = 0;

#ifndef PTSAN_MUSL_INTERCEPTION
  // Musl has already registered these ranges in its pre-TLS startup wrapper.
  InitializeSharedConstantRanges();
#endif
  // Musl has TLS by this point. Install the thread record
  // before returning to instrumented startup code, including va_start users.
#ifndef PTSAN_MUSL_INTERCEPTION
  PTSanTSDInit(PTSanTSDDtor);
#endif

  // TLS exists at this point. Publish the main thread before constructors or
  // subsequent instrumented-libc setup can request ordinary stack/heap IDs.
  PTSanThread *main_thread = PTSanThread::Create(nullptr, nullptr);
  SetCurrentThread(main_thread);
}

static void ptsanInitFinish() {
  Atexit(ptsan_atexit);

  ptsan_init_is_running = false;
  ptsan_initialized = true;
#ifndef PTSAN_MUSL_INTERCEPTION
  // ptsan is initialized, but there are potentially libc calls between
  // here an main (in c++ at least). PointerTagSanitizer will add a
  // call to ptsan_enable_libc_instr in main, which will set this back to
  // zero
  ptsan_in_libc++;
#endif
  //Printf("Done initializing ptsan\n");
}

SANITIZER_INTERFACE_ATTRIBUTE void __ptsan_init() {
  CHECK(!ptsan_init_is_running);
  if (ptsan_initialized)
    return;

  ptsanInitStart();
#ifndef PTSAN_MUSL_INTERCEPTION
  ptsanInitFinish();
#endif
}

#ifdef PTSAN_MUSL_INTERCEPTION
// An instrumented libc needs its PTSan-generated global proxy constructors to
// run after the ID tables exist but before initialization calls into libc.
SANITIZER_INTERFACE_ATTRIBUTE void __ptsan_finish_instrumented_libc_init() {
  CHECK(ptsan_init_is_running);
  CHECK(!ptsan_initialized);
  ptsanInitFinish();
}
#endif
}
