//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -pthread %s -o %t
// RUN: %t return
// RUN: %t exit
// RUN: %t detach
// RUN: %t cancel
// RUN: %t last 2>&1 | FileCheck %s
// CHECK: last-thread atexit completed

// Musl uses its pthread_exit frame after running all TSD destructors. Force
// that frame to allocate slow stack IDs, which must remain live until the
// final exit syscall (or __unmapself for a detached thread). Check that IDs
// are still reclaimed, and that the last thread retains TLS through atexit.
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

extern __thread uint16_t ptsan_next_stack_id, ptsan_stack_id_limit;
extern __thread uint16_t ptsan_pending_stack_ids;
extern volatile uint64_t __ptsan_used_stack_ids;

// Runtime metadata does not need application TLS/global IDs.
__attribute__((noinline, disable_sanitizer_instrumentation))
static void force_slow_stack_ids(void) {
  ptsan_next_stack_id = ptsan_stack_id_limit;
  ptsan_pending_stack_ids = 0;
}

__attribute__((noinline, disable_sanitizer_instrumentation))
static uint64_t live_stack_ids(void) {
  return __atomic_load_n(&__ptsan_used_stack_ids, __ATOMIC_RELAXED);
}

static char mode;
static pthread_key_t key;
static pthread_barrier_t barrier;
static int destructor_calls;
static _Thread_local int value = 17;
static int *retained;

static void destroy(void *arg) {
  assert(arg == &value);
  assert(*(volatile int *)arg == value);
  value = 23;
  __atomic_fetch_add(&destructor_calls, 1, __ATOMIC_RELAXED);
}

static void *worker(void *unused) {
  assert(pthread_setspecific(key, &value) == 0);
  pthread_barrier_wait(&barrier);
  force_slow_stack_ids();
  if (mode == 'c') {
    // Deferred self-cancellation exercises the exit path without depending
    // on instrumentation of the kernel-provided cancellation signal frame.
    assert(pthread_cancel(pthread_self()) == 0);
    pthread_testcancel();
    abort();
  }
  if (mode == 'e')
    pthread_exit(0);
  return 0;
}

static void report_exit(void) {
  assert(value == 23);
  assert(__atomic_load_n(&destructor_calls, __ATOMIC_RELAXED) == 1);
  assert(*(volatile int *)retained == 23);
  fputs("last-thread atexit completed\n", stderr);
}

int main(int argc, char **argv) {
  assert(argc == 2);
  mode = argv[1][0];
  assert(pthread_key_create(&key, destroy) == 0);
  if (mode == 'l') {
    assert(atexit(report_exit) == 0);
    assert(pthread_setspecific(key, &value) == 0);
    retained = &value;
    force_slow_stack_ids();
    pthread_exit(0);
  }

  assert(pthread_barrier_init(&barrier, 0, 2) == 0);
  uint64_t before = live_stack_ids();
  pthread_t thread;
  assert(pthread_create(&thread, 0, worker, 0) == 0);
  if (mode == 'd')
    assert(pthread_detach(thread) == 0);
  pthread_barrier_wait(&barrier);
  if (mode == 'd') {
    // Wait for the actual unmap, not just the last user callback.
    size_t page_size = sysconf(_SC_PAGESIZE);
    void *page = (void *)((uintptr_t)thread & -(uintptr_t)page_size);
    unsigned char resident;
    while (mincore(page, page_size, &resident) == 0)
      sched_yield();
    assert(errno == ENOMEM);
  } else {
    void *result;
    assert(pthread_join(thread, &result) == 0);
    assert(result == (mode == 'c' ? PTHREAD_CANCELED : 0));
  }
  assert(__atomic_load_n(&destructor_calls, __ATOMIC_RELAXED) == 1);
  assert(live_stack_ids() == before);
  assert(pthread_barrier_destroy(&barrier) == 0);
  assert(pthread_key_delete(key) == 0);
  return 0;
}
