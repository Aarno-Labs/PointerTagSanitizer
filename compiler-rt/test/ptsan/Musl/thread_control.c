//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict %s -pthread -o %t
// RUN: %t

#include <assert.h>
#include <locale.h>
#include <pthread.h>
#include <stdint.h>

static _Thread_local int value;

int main(void) {
  pthread_t self = pthread_self();
  assert((uintptr_t)self >> 48);
  assert(pthread_equal(self, pthread_self()));

  pthread_key_t key;
  assert(pthread_key_create(&key, 0) == 0);
  assert(pthread_setspecific(key, &value) == 0);
  assert(pthread_getspecific(key) == &value);
  *(int *)pthread_getspecific(key) = 42;
  assert(value == 42);
  assert(pthread_key_delete(key) == 0);
  assert(uselocale(0) == LC_GLOBAL_LOCALE);
  return 0;
}
