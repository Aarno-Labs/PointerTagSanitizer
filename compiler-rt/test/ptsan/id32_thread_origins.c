//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: split-file %s %t.dir
// RUN: %clang_raw -O2 -pthread -c %t.dir/threads.c -o %t.threads.o
// RUN: %clang_ptsan_id32 -O2 -pthread -mllvm -ptsan-random-seed=1 %t.dir/main.c %t.threads.o -o %t
// RUN: %run %t

// An explicit seed starts the same independent sequence in each thread.
// Interleave requests to catch a shared generator. Keep the workers
// uninstrumented so stack registration does not consume extra random values.

//--- threads.c
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

extern uint32_t ptsan_choose_stack_origin(uint64_t);

static pthread_barrier_t barrier;

static void *sample_origins(void *arg) {
  uint32_t *origins = arg;
  for (unsigned i = 0; i < 8; ++i) {
    pthread_barrier_wait(&barrier);
    origins[i] = ptsan_choose_stack_origin(16);
  }
  return NULL;
}

void check_thread_origins(void) {
  pthread_t threads[2];
  uint32_t origins[2][8];
  assert(pthread_barrier_init(&barrier, NULL, 2) == 0);
  for (unsigned i = 0; i < 2; ++i) {
    assert(pthread_create(&threads[i], NULL, sample_origins, origins[i]) == 0);
  }
  for (unsigned i = 0; i < 2; ++i) {
    assert(pthread_join(threads[i], NULL) == 0);
  }
  assert(pthread_barrier_destroy(&barrier) == 0);
  assert(memcmp(origins[0], origins[1], sizeof(origins[0])) == 0);
  assert(origins[0][0] != origins[0][1]);
}

//--- main.c
void check_thread_origins(void);

int main(void) {
  check_thread_origins();
  return 0;
}
