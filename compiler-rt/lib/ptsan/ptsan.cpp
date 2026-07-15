#include "ptsan.h"
#include "interceptors/ptsan_interceptors.h"
#include "ptsan_thread.h"
#include "ptsan_bounds.h"
#include "ptsan_stats.h"

#include <pthread.h>
#include <stdint.h>
#include <sys/syscall.h>
#if defined(__x86_64__)
#include <asm/prctl.h>  // x86-only (arch_prctl constants for LAM)
#endif

#include "sanitizer_common/sanitizer_linux.h"
#include "sanitizer_common/sanitizer_file.h"

using namespace __ptsan;

bool __ptsan::ptsan_initialized;
bool __ptsan::ptsan_init_is_running;


namespace __ptsan {
// Thread interception
static pthread_key_t tsd_key;
static bool tsd_key_inited = false;

void PTSanTSDInit(void (*destructor)(void *tsd)) {
  CHECK(!tsd_key_inited);
  tsd_key_inited = true;
  CHECK_EQ(0, pthread_key_create(&tsd_key, destructor));
}

static THREADLOCAL PTSanThread* ptsan_current_thread;

PTSanThread *GetCurrentThread() {
  return ptsan_current_thread;
}

void SetCurrentThread(PTSanThread *t) {
  // Make sure we do not reset the current PTSanThread.
  CHECK_EQ(0, ptsan_current_thread);
  ptsan_current_thread = t;

  if (t->stack_id_bucket_no >= 0) {
    ptsan_next_stack_id = StackIDBuckets[t->stack_id_bucket_no].start;
    ptsan_stack_id_limit = StackIDBuckets[t->stack_id_bucket_no].end;
  } else {
    // 1 > 0, so we always go to the slow case
    ptsan_next_stack_id = 1;
    ptsan_stack_id_limit = 0;
  }

  // Make sure that PTSanTSDDtor gets called at the end.
  CHECK(tsd_key_inited);
  pthread_setspecific(tsd_key, (void *)t);
}

void PTSanTSDDtor(void *tsd) {
  PTSanThread *t = (PTSanThread*)tsd;
  if (t->destructor_iterations_ > 1) {
    t->destructor_iterations_--;
    CHECK_EQ(0, pthread_setspecific(tsd_key, tsd));
    return;
  }
  ScopedBlockSignals block(nullptr);
  ptsan_current_thread = nullptr;
  // Make sure that signal handler can not see a stale current thread pointer.
  atomic_signal_fence(memory_order_seq_cst);
  PTSanThread::TSDDtor(tsd);
}
} // namespace __ptsan


void ValidateCls() {
  // If you hit these, go change the define in ptsan.h
  #ifdef PTSAN_NO_IDS
    CHECK(ClNoIDs);
  #else
    CHECK(!ClNoIDs);
  #endif

  #ifdef PTSAN_NO_STRIP
    CHECK(ClNoStrip);
  #else
    CHECK(!ClNoStrip);
  #endif

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

SANITIZER_INTERFACE_ATTRIBUTE void __ptsan_init() {
  CHECK(!ptsan_init_is_running);
  if (ptsan_initialized)
    return;
  ptsan_init_is_running = true;

  ValidateCls();

  if (ClLAM)
    ptsan_enable_lam_u48();

  //Printf("Starting intialization of ptsan\n");

  InitializeInterceptors();

  PTSanTSDInit(PTSanTSDDtor);

  InitializeIDs();

  PTSanThread *main_thread = PTSanThread::Create(nullptr, nullptr);
  SetCurrentThread(main_thread);
  main_thread->Init();

  Atexit(ptsan_atexit);

  ptsan_init_is_running = false;
  ptsan_initialized = true;
  // ptsan is initialized, but there are potentially libc calls between
  // here an main (in c++ at least). PointerTagSanitizer will add a
  // call to ptsan_enable_libc_instr in main, which will set this back to
  // zero
  ptsan_in_libc++;
  //Printf("Done initializing ptsan\n");
}
}
