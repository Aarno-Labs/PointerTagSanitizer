//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: split-file %s %t
// RUN: %ptsan_musl_strict -pthread -o %t/test \
// RUN:   %t/main.c %t/access.c
// RUN: %t/test
// RUN: not %t/test stale 2>&1 | FileCheck %s
// CHECK: Found bad POINT access

// Keep both workers alive while their IDs are compared. Check ID stability
// across TUs and a pthread-key destructor, then retirement after thread exit.
//--- main.c
#include <assert.h>
#include <pthread.h>
#include <stdint.h>

_Thread_local int value = 7;
int *get_value(void);
int read_value(volatile int *p);
static pthread_barrier_t barrier;
static pthread_key_t key;

struct result {
  int *pointer;
  int destructor_ran;
};

static void destroy(void *arg) {
  struct result *result = arg;
  assert(read_value(get_value()) == 23);
  assert(((uintptr_t)get_value() >> 48) ==
         ((uintptr_t)result->pointer >> 48));
  ++result->destructor_ran;
#ifdef REPEAT_TLS_DTOR
  if (result->destructor_ran < 4) {
    assert(pthread_setspecific(key, result) == 0);
  }
#endif
}

static void *worker(void *arg) {
  struct result *result = arg;
  assert(read_value(get_value()) == 7);
  value = 23;
  result->pointer = get_value();
  assert(((uintptr_t)&value >> 48) ==
         ((uintptr_t)result->pointer >> 48));
  assert(pthread_setspecific(key, result) == 0);
  pthread_barrier_wait(&barrier);
  pthread_barrier_wait(&barrier);
  return 0;
}

int main(int argc, char **argv) {
  assert(pthread_barrier_init(&barrier, 0, 3) == 0);
  assert(pthread_key_create(&key, destroy) == 0);
  struct result results[2] = {0};
  pthread_t threads[2];
  for (int i = 0; i < 2; ++i) {
    assert(pthread_create(&threads[i], 0, worker, &results[i]) == 0);
  }
  pthread_barrier_wait(&barrier);
  uintptr_t main_id = (uintptr_t)get_value() >> 48;
  uintptr_t first_id = (uintptr_t)results[0].pointer >> 48;
  uintptr_t second_id = (uintptr_t)results[1].pointer >> 48;
  assert(main_id && first_id && second_id);
  assert(main_id != first_id && main_id != second_id && first_id != second_id);
  assert(read_value(get_value()) == 7);
  // Another thread may read a live TLS object through its escaped pointer.
  assert(read_value(results[0].pointer) == 23);
  assert(read_value(results[1].pointer) == 23);
  pthread_barrier_wait(&barrier);
  for (int i = 0; i < 2; ++i) {
    assert(pthread_join(threads[i], 0) == 0);
#ifdef REPEAT_TLS_DTOR
    assert(results[i].destructor_ran == 4);
#else
    assert(results[i].destructor_ran);
#endif
  }
  if (argc > 1) {
    return read_value(results[0].pointer);
  }
  assert(pthread_barrier_destroy(&barrier) == 0);
  return 0;
}

//--- access.c
extern _Thread_local int value;

int *get_value(void) {
  return &value;
}

int read_value(volatile int *p) {
  return *p;
}
