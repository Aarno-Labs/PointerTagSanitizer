//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -pthread %s -o %t
// RUN: %t no-guard
// RUN: %t guard
//
// Exercise page rounding with translated offsets, not just ID16 bit stripping.
// RUN: %ptsan_musl_id32_strict -pthread -mllvm -ptsan-random-seed=123456789 %s -o %t.id32
// RUN: %t.id32 no-guard
// RUN: %t.id32 guard

// Reduced from Fleetbench RPC. Mapping IDs preserve page alignment so musl's
// mprotect.c can round encoded addresses with ordinary instrumentation. Adding
// -ptsan-strip-ptrtoint to that TU decodes too early: the syscall handler decodes
// the raw address again in strict ID32 mode, causing ENOMEM. pthread_create
// reports EAGAIN. The no-guard case avoids that mprotect call.

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

static void *worker(void *argument) {
  return argument;
}

int main(int argc, char **argv) {
  assert(argc == 2);
  assert(argv[1][0] == 'n' || argv[1][0] == 'g');
  long page_size = sysconf(_SC_PAGESIZE);
  assert(page_size > 0);
  size_t guard_size = argv[1][0] == 'g' ? (size_t)page_size : 0;

  pthread_attr_t attributes;
  assert(pthread_attr_init(&attributes) == 0);
  assert(pthread_attr_setguardsize(&attributes, guard_size) == 0);

  int value = 42;
  pthread_t thread;
  int error = pthread_create(&thread, &attributes, worker, &value);
  if (error != 0) {
    fprintf(stderr, "pthread_create with guard size %zu returned %d\n",
            guard_size, error);
  }
  assert(error == 0);

  void *result;
  assert(pthread_join(thread, &result) == 0);
  assert(result == &value);
  assert(pthread_attr_destroy(&attributes) == 0);
  return 0;
}
