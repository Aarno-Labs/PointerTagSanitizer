// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan -O2 -pthread -mllvm -ptsan-abort-on-error %s -o %t
// RUN: %run %t

#include <assert.h>
#include <pthread.h>
#include <stdint.h>

extern volatile uint64_t runtime_thread_count
    __asm__("_ZN7__ptsan11num_threadsE");
extern __thread uint16_t ptsan_stack_id_limit;
static _Thread_local int value = 7;

// Read runtime bookkeeping without requiring instrumentation for its globals.
__attribute__((noinline, disable_sanitizer_instrumentation))
static uint64_t thread_count(void) {
  return __atomic_load_n(&runtime_thread_count, __ATOMIC_RELAXED);
}

__attribute__((noinline, disable_sanitizer_instrumentation))
static uint16_t bucket_limit(void) {
  return ptsan_stack_id_limit;
}

__attribute__((noinline)) static int read_value(volatile int *p) {
  return *p;
}

static void *worker(void *arg) {
  // Failed creations must not consume the available fast-stack buckets.
  assert(bucket_limit() != 0);
  assert(value == 7);
  value = 19;
  return arg;
}

int main(void) {
  uint64_t initial_count = thread_count();
  int *saved_tls = &value;
  value = 42;

  pthread_attr_t attr;
  assert(pthread_attr_init(&attr) == 0);
  // Both supported 64-bit targets accept this attribute, but cannot map a
  // stack this large. This forces failure without depending on privileges.
  assert(pthread_attr_setstacksize(&attr, (size_t)PTRDIFF_MAX / 2) == 0);
  for (int i = 0; i < 32; ++i) {
    pthread_t thread;
    assert(pthread_create(&thread, &attr, worker, saved_tls) != 0);
    assert(thread_count() == initial_count);
    // Failed-child cleanup must leave the parent's TLS and its bounds live.
    assert(read_value(saved_tls) == 42);
  }
  assert(pthread_attr_destroy(&attr) == 0);

  pthread_t thread;
  void *result;
  assert(pthread_create(&thread, 0, worker, saved_tls) == 0);
  assert(pthread_join(thread, &result) == 0);
  assert(result == saved_tls);
  assert(read_value(saved_tls) == 42);
  assert(thread_count() == initial_count);
  return 0;
}
