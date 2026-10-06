//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// XFAIL: *
// REQUIRES: x86_64-target-arch
// RUN: split-file %s %t
// RUN: %clang_raw -O2 -fPIC -shared %t/early.c -o %t/libearly.so
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error %t/main.c -pthread -rdynamic -L%t -Wl,-rpath,%t -Wl,--no-as-needed -learly -o %t/test
// RUN: %t/test

// A startup dependency's constructor runs before the executable's constructors.
// A thread it creates therefore misses the executable's TLS registration.
// Wait for that thread to finish runtime startup before returning from the
// constructor, then check its TLS ID after main's own TLS has been initialized.
// The synchronization harness is uninstrumented because glibc barrier wrappers
// are missing; get_value exercises the actual instrumented TLS access path.

//--- early.c
void start_thread(void);

__attribute__((constructor)) static void initialize(void) {
  start_thread();
}

//--- main.c
#include <assert.h>
#include <pthread.h>
#include <stdint.h>

_Thread_local int value = 7;
static pthread_t thread;
static pthread_barrier_t ready, release;

__attribute__((noinline)) int *get_value(void) {
  return &value;
}

__attribute__((disable_sanitizer_instrumentation))
static void *worker(void *arg) {
  pthread_barrier_wait(&ready);
  pthread_barrier_wait(&release);
  unsigned id = (uintptr_t)get_value() >> 48;
  assert(id && id != 0xffff);
  return 0;
}

__attribute__((disable_sanitizer_instrumentation))
void start_thread(void) {
  assert(pthread_barrier_init(&ready, 0, 2) == 0);
  assert(pthread_barrier_init(&release, 0, 2) == 0);
  assert(pthread_create(&thread, 0, worker, 0) == 0);
  pthread_barrier_wait(&ready);
}

__attribute__((disable_sanitizer_instrumentation))
int main(void) {
  unsigned id = (uintptr_t)get_value() >> 48;
  assert(id && id != 0xffff);
  pthread_barrier_wait(&release);
  assert(pthread_join(thread, 0) == 0);
  assert(pthread_barrier_destroy(&ready) == 0);
  assert(pthread_barrier_destroy(&release) == 0);
  return 0;
}
