//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan -O2 -pthread -mllvm -ptsan-abort-on-error -o %t %s
// RUN: %run %t 2>&1 | FileCheck %s

// RUN: %clang_ptsan_id32 -O2 -pthread -mllvm -ptsan-abort-on-error -o %t %s
// RUN: %run %t 2>&1 | FileCheck %s

// A tagged control pointer must be decoded before entering native libc.
// The initializer must still allocate tagged application memory.
// CHECK-NOT: Found bad
// CHECK: pthread-once-ok
// CHECK-NOT: Found bad

#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>

static pthread_once_t once = PTHREAD_ONCE_INIT;
static int calls;
static int *value;

static void initialize(void) {
  ++calls;
  value = malloc(sizeof(*value));
  assert(value && get_id(value) != 0);
  *value = 42;
}

static void *worker(void *unused) {
  assert(pthread_once(&once, initialize) == 0);
  assert(*value == 42);
  return NULL;
}

int main(void) {
  pthread_t threads[4];
  assert(get_id(&once) != 0);
  for (int i = 0; i < 4; ++i)
    assert(pthread_create(&threads[i], NULL, worker, NULL) == 0);
  for (int i = 0; i < 4; ++i)
    assert(pthread_join(threads[i], NULL) == 0);
  assert(pthread_once(&once, initialize) == 0);
  assert(calls == 1);
  assert(*value == 42);
  free(value);
  puts("pthread-once-ok");
  return 0;
}
