//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -pthread -o cleanup-o2 %s
// RUN: ./cleanup-o2

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

static atomic_int cleanup_installed;
static atomic_int cleanup_ran;

static void cleanup(void *arg) {
  atomic_store_explicit(&cleanup_ran, 1, memory_order_relaxed);
  if (pthread_mutex_unlock(arg) != 0)
    abort();
}

static void *worker(void *arg) {
  pthread_mutex_t *mutex = arg;

  if (pthread_mutex_lock(mutex) != 0)
    abort();

  pthread_cleanup_push(cleanup, mutex);
  atomic_store_explicit(&cleanup_installed, 1, memory_order_release);

  for (;;)
    pthread_testcancel();

  pthread_cleanup_pop(1);
  return 0;
}

int main(void) {
  pthread_mutex_t mutex;
  pthread_t thread;
  void *result = 0;

  if (pthread_mutex_init(&mutex, 0) != 0)
    abort();
  if (pthread_create(&thread, 0, worker, &mutex) != 0)
    abort();

  while (!atomic_load_explicit(&cleanup_installed, memory_order_acquire))
    ;

  if (pthread_cancel(thread) != 0)
    abort();
  if (pthread_join(thread, &result) != 0)
    abort();
  if (result != PTHREAD_CANCELED)
    abort();
  if (!atomic_load_explicit(&cleanup_ran, memory_order_relaxed))
    abort();

  if (pthread_mutex_trylock(&mutex) != 0)
    abort();
  if (pthread_mutex_unlock(&mutex) != 0)
    abort();
  if (pthread_mutex_destroy(&mutex) != 0)
    abort();

  return 0;
}
