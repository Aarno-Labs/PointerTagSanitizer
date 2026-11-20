//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -pthread %s -o %t
// RUN: %run %t
// RUN: %run %t nested
// RUN: %ptsan_musl_strict_regular -pthread \
// RUN:   -DHEAP_CACHE_NO_STATS %s -o %t.regular
// RUN: %run %t.regular
// RUN: %run %t.regular nested
//
// Exercise shared nonstack/slow-stack ID caches, using raw storage as synthetic
// objects. Calling malloc/free from a handler would test libc reentrancy, which
// is not supported. No special runtime, timing hooks, or signal-unsafe allocator
// calls are needed here. Both returning and siglongjmp handlers use the caches;
// jumps happen after completed operations, not in the accepted abandonment gap.
#define _GNU_SOURCE
#include "Inputs/signal_test.h"
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <time.h>

extern uint16_t ptsan_get_next_nonstack_id(void *, uint64_t, uint8_t, uint64_t);
extern void ptsan_mark_ptr_freed(void *, uint8_t);
extern void ptsan_get_stack_ids(uint16_t *, uint64_t);
extern void ptsan_free_stack_ids(uintptr_t);
extern uint16_t ptsan_get_repeatable_alloca_id(void *, uint64_t, uint64_t);
#ifndef HEAP_CACHE_NO_STATS
extern volatile uint64_t __ptsan_used_ids;
#endif

static pthread_t receiver;
static int stop;
static volatile sig_atomic_t nested;
static volatile sig_atomic_t depth;
static volatile sig_atomic_t jumping;
static sigjmp_buf destination;
static unsigned deliveries, interruptions;
static uintptr_t owners[65536];
static volatile sig_atomic_t exercising;

static SIGNAL_RAW uint64_t live_ids(void) {
#ifdef HEAP_CACHE_NO_STATS
  return 0;
#else
  return __atomic_load_n(&__ptsan_used_ids, __ATOMIC_RELAXED);
#endif
}

static SIGNAL_RAW void exercise(unsigned count) {
  // Cross the 64-ID signal-cache boundary with all three allocation paths.
  char objects[513];
  uintptr_t tagged[513];
  uintptr_t repeated[513];
  uint16_t stack_ids[513];
  uintptr_t cursor = ptsan_stack_id_log.cursor;
  ptsan_get_stack_ids(stack_ids, count);
  for (unsigned i = 0; i != count; ++i) {
    uint16_t id = stack_ids[i];
    uintptr_t owner = (uintptr_t)&stack_ids[i] | ((uintptr_t)id << 48);
    uintptr_t empty = 0;
    SIGNAL_CHECK(id && id != ptsan_default_id());
    int claimed = __atomic_compare_exchange_n(&owners[id], &empty, owner, 0,
                                              __ATOMIC_RELAXED,
                                              __ATOMIC_RELAXED);
    SIGNAL_CHECK(claimed);
  }
  for (unsigned i = 0; i != count; ++i) {
    uint16_t id = ptsan_get_next_nonstack_id(&objects[i], 1, i % 3, 1);
    SIGNAL_CHECK(id && id != ptsan_default_id());
    tagged[i] = (uintptr_t)&objects[i] | ((uintptr_t)id << 48);
    uintptr_t empty = 0;
    int claimed = __atomic_compare_exchange_n(&owners[id], &empty, tagged[i], 0,
                                              __ATOMIC_RELAXED,
                                              __ATOMIC_RELAXED);
    SIGNAL_CHECK(claimed);
    id = ptsan_get_repeatable_alloca_id(&objects[i], 1, 1);
    SIGNAL_CHECK(id && id != ptsan_default_id());
    repeated[i] = (uintptr_t)&objects[i] | ((uintptr_t)id << 48);
    empty = 0;
    claimed = __atomic_compare_exchange_n(&owners[id], &empty, repeated[i], 0,
                                          __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    SIGNAL_CHECK(claimed);
  }
  for (unsigned i = 0; i != count; ++i) {
    uint16_t id = get_id(tagged[i]);
    SIGNAL_CHECK(ptsan_get_base(id) == tagged[i] && ptsan_get_size(id) == 1);
    uintptr_t owner = __atomic_exchange_n(&owners[id], 0, __ATOMIC_RELAXED);
    SIGNAL_CHECK(owner == tagged[i]);
    ptsan_mark_ptr_freed((void *)tagged[i], i % 3);
    SIGNAL_CHECK(ptsan_get_size(id) == 0);
    id = get_id(repeated[i]);
    SIGNAL_CHECK(ptsan_get_base(id) == repeated[i] && ptsan_get_size(id) == 1);
    owner = __atomic_exchange_n(&owners[id], 0, __ATOMIC_RELAXED);
    SIGNAL_CHECK(owner == repeated[i]);
  }
  for (unsigned i = 0; i != count; ++i) {
    uint16_t id = stack_ids[i];
    uintptr_t owner = (uintptr_t)&stack_ids[i] | ((uintptr_t)id << 48);
    uintptr_t previous = __atomic_exchange_n(&owners[id], 0, __ATOMIC_RELAXED);
    SIGNAL_CHECK(previous == owner);
  }
  ptsan_free_stack_ids(cursor);
  SIGNAL_CHECK(ptsan_stack_id_log.cursor == cursor);
}

static void handler(int signo) {
  SIGNAL_CHECK(signo == SIGUSR1);
  ++depth;
  exercise(257);
  if (nested && depth == 1) {
    // SA_NODEFER permits actual nesting. Block further deliveries at depth 2
    // to keep stress from overflowing the native stack.
    SIGNAL_CHECK(raise(SIGUSR1) == 0);
  }
  exercise(513);
  --depth;
  __atomic_fetch_add(&deliveries, 1, __ATOMIC_RELAXED);
  if (exercising) {
    __atomic_fetch_add(&interruptions, 1, __ATOMIC_RELAXED);
  }
  if (jumping) {
    siglongjmp(destination, 1);
  }
}

static void nested_handler(int signo) {
  if (depth) {
    sigset_t mask;
    SIGNAL_CHECK(sigemptyset(&mask) == 0);
    SIGNAL_CHECK(sigaddset(&mask, SIGUSR1) == 0);
    SIGNAL_CHECK(sigprocmask(SIG_BLOCK, &mask, 0) == 0);
  }
  handler(signo);
}

static void *sender(void *unused) {
  (void)unused;
  const struct timespec pause = {0, 100000};
  for (unsigned i = 0; i != 4000 &&
                       !__atomic_load_n(&stop, __ATOMIC_ACQUIRE); ++i) {
    SIGNAL_CHECK(pthread_kill(receiver, SIGUSR1) == 0);
    SIGNAL_CHECK(nanosleep(&pause, 0) == 0);
  }
  return 0;
}

int main(int argc, char **argv) {
  (void)argv;
  nested = argc > 1;
  struct sigaction action = {0};
  action.sa_handler = nested ? nested_handler : handler;
  action.sa_flags = nested ? SA_NODEFER : 0;
  SIGNAL_CHECK(sigemptyset(&action.sa_mask) == 0);
  SIGNAL_CHECK(sigaction(SIGUSR1, &action, 0) == 0);
  receiver = pthread_self();
  pthread_t thread;
  // Include asynchronous stress in the leak check. Take the baseline before
  // creating the sender and compare after joining it, so its live IDs are
  // absent from both measurements.
  uint64_t baseline = live_ids();
  SIGNAL_CHECK(pthread_create(&thread, 0, sender, 0) == 0);
  while (!__atomic_load_n(&deliveries, __ATOMIC_RELAXED)) {
    sched_yield();
  }
  exercising = 1;
  for (unsigned i = 0; i != 4000; ++i) {
    exercise(255 + (i % 3));
  }
  exercising = 0;
  __atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
  SIGNAL_CHECK(pthread_join(thread, 0) == 0);
  SIGNAL_CHECK(__atomic_load_n(&interruptions, __ATOMIC_RELAXED) > 0);
  SIGNAL_CHECK(live_ids() == baseline);

  // Check exact live counts after joining the sender, with no concurrent libc
  // allocation. Completed handler transactions must survive repeated escapes.
  nested = 0;
  for (unsigned i = 0; i != 1024; ++i) {
    if (sigsetjmp(destination, 1) == 0) {
      jumping = 1;
      SIGNAL_CHECK(raise(SIGUSR1) == 0);
      SIGNAL_CHECK(0);
    }
    jumping = 0;
    SIGNAL_CHECK(live_ids() == baseline);
    exercise(513);
  }
  sigset_t mask;
  SIGNAL_CHECK(sigprocmask(SIG_SETMASK, 0, &mask) == 0);
  SIGNAL_CHECK(sigismember(&mask, SIGUSR1) == 0);
  for (unsigned i = 0; i != 65536; ++i) {
    SIGNAL_CHECK(__atomic_load_n(&owners[i], __ATOMIC_RELAXED) == 0);
  }
  return 0;
}
