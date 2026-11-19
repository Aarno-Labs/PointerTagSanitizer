//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict %s -pthread -o %t
// RUN: %t

#include <assert.h>
#include <pthread.h>

static void *worker(void *argument) { return argument; }

int main(void) {
  pthread_t thread;
  assert(pthread_create(&thread, 0, worker, 0) == 0);
  assert(pthread_join(thread, 0) == 0);

  pthread_key_t first, second;
  assert(pthread_key_create(&first, 0) == 0);
  assert(pthread_key_create(&second, 0) == 0);

  int value = 42;
  assert(pthread_setspecific(second, &value) == 0);
  assert(pthread_getspecific(second) == &value);

  assert(pthread_key_delete(second) == 0);
  assert(pthread_key_delete(first) == 0);
  return 0;
}
