//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -pthread %s -o %t
// RUN: %t join
// RUN: %t detach
// RUN: not %t join stale 2>&1 | FileCheck %s
// RUN: not %t detach stale 2>&1 | FileCheck %s
// CHECK: Found bad POINT access

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

static pthread_barrier_t barrier;
static pthread_t self;

static void *worker(void *arg) {
  self = pthread_self();
  assert((uintptr_t)self >> 48);
  pthread_barrier_wait(&barrier);
  pthread_barrier_wait(&barrier);
  return arg;
}

__attribute__((noinline)) static int read_byte(volatile char *p) {
  return *p;
}

int main(int argc, char **argv) {
  assert(argc >= 2);
  int detached = argv[1][0] == 'd';
  assert(pthread_barrier_init(&barrier, 0, 2) == 0);
  int argument = 42;
  pthread_t thread;
  assert(pthread_create(&thread, 0, worker, &argument) == 0);
  pthread_barrier_wait(&barrier);
  assert(pthread_equal(thread, self));
  if (detached) {
    assert(pthread_detach(thread) == 0);
  }
  pthread_barrier_wait(&barrier);

  if (detached) {
    // Wait for __unmapself, not just for the last user instruction. mincore
    // queries mappings without reading the potentially unmapped memory.
    size_t page_size = sysconf(_SC_PAGESIZE);
    void *page = (void *)((uintptr_t)thread & -(uintptr_t)page_size);
    unsigned char resident;
    while (mincore(page, page_size, &resident) == 0) {
      sched_yield();
    }
    assert(errno == ENOMEM);
  } else {
    void *result;
    assert(pthread_join(thread, &result) == 0);
    assert(result == &argument);
    assert(*(int *)result == 42);
  }
  assert(pthread_barrier_destroy(&barrier) == 0);
  if (argc > 2) {
    return read_byte((char *)thread);
  }
  return 0;
}
