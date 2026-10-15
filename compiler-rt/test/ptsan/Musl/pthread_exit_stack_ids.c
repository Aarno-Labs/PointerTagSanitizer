//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -pthread \
// RUN:   -mllvm -ptsan-no-stack-id-fast-path %s -o %t
// RUN: %run %t 2>&1 | FileCheck %s
// RUN: %run %t exit 2>&1 | FileCheck %s
// CHECK: thread stack IDs reclaimed

// Returning from an instrumented worker reclaims its frame IDs. Musl's
// pthread_exit bypasses that cleanup, so thread teardown must recycle the
// outstanding slow stack IDs and the IDs held by repeatable-alloca containers.
// Check both kinds independently.
//
// Use the validation runtime's live stack-ID count so this stays small and
// checks reclamation without depending on ID reuse order or cleared bounds.
// The normal-return run is a control for reclamation from abandoned frames.
#include "../Inputs/ptsan_test_interface.h"
#include <alloca.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern volatile uint64_t __ptsan_used_stack_ids;

// Runtime metadata is not an application global requiring its own proxy.
__attribute__((noinline, disable_sanitizer_instrumentation))
static uint64_t live_stack_ids(void) {
  return __atomic_load_n(&__ptsan_used_stack_ids, __ATOMIC_RELAXED);
}

static int exit_without_return;
static volatile unsigned allocation_size = 37;
static volatile unsigned allocation_count = 3;

__attribute__((noinline)) static void consume(char *p, unsigned size) {
  unsigned id = get_id(p);
  if (!id || id == ptsan_default_id())
    _Exit(2);
  *(volatile char *)p = 1;
  *(volatile char *)(p + size - 1) = 2;
}

static void *fixed_worker(void *unused) {
  char buffer[37];
  consume(buffer, sizeof(buffer));
  if (exit_without_return)
    pthread_exit(0);
  return 0;
}

static void *repeatable_worker(void *unused) {
  unsigned size = allocation_size;
  unsigned count = allocation_count;
  for (unsigned i = 0; i < count; ++i)
    consume(alloca(size), size);
  if (exit_without_return)
    pthread_exit(0);
  return 0;
}

static int check_worker(void *(*worker)(void *), const char *name) {
  pthread_t thread;
  uint64_t before = live_stack_ids();
  if (pthread_create(&thread, 0, worker, 0) || pthread_join(thread, 0))
    return 1;
  uint64_t after = live_stack_ids();
  if (after != before) {
    fprintf(stderr, "%s: live stack IDs before=%llu after=%llu\n", name,
            (unsigned long long)before, (unsigned long long)after);
    return 1;
  }
  return 0;
}

int main(int argc, char **argv) {
  exit_without_return = argc > 1;
  int failed = check_worker(fixed_worker, "fixed");
  failed |= check_worker(repeatable_worker, "repeatable");
  if (!failed)
    fputs("thread stack IDs reclaimed\n", stderr);
  return failed;
}
