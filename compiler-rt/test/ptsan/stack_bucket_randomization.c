//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Fast-stack buckets are randomly reserved across the ID namespace, and each
// thread begins its bucket search at a random reservation. Turning ID
// randomization off retains the old contiguous, lowest-available assignment.
// RUN: %clang_ptsan -O2 -pthread -mllvm -ptsan-random-seed=1 -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-id-bucket-size=64 -mllvm -ptsan-stack-id-thread-buckets=16 %s -o %t
// RUN: %run %t
// RUN: %clang_ptsan -O2 -pthread -mllvm -ptsan-random-seed=1 -mllvm -ptsan-randomize-ids=0 -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-id-bucket-size=64 -mllvm -ptsan-stack-id-thread-buckets=16 -DSEQUENTIAL %s -o %t.sequential
// RUN: %run %t.sequential
// RUN: %clang_ptsan_id32 -O2 -pthread -mllvm -ptsan-random-seed=1 -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-id-bucket-size=64 -mllvm -ptsan-stack-id-thread-buckets=16 %s -o %t.id32
// RUN: %run %t.id32
// RUN: %clang_ptsan_id32 -O2 -pthread -mllvm -ptsan-random-seed=1 -mllvm -ptsan-randomize-ids=0 -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-id-bucket-size=64 -mllvm -ptsan-stack-id-thread-buckets=16 -DSEQUENTIAL %s -o %t.id32.sequential
// RUN: %run %t.id32.sequential

#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>

enum { BucketSize = 64, ThreadCount = 8 };
static PTSanID IDs[ThreadCount];
static atomic_uint Ready;
static atomic_uint Release;

static void *record_stack_id(void *Argument) {
  unsigned Index = (unsigned)(uintptr_t)Argument;
  int Local;
  IDs[Index] = get_id(&Local);
  atomic_fetch_add_explicit(&Ready, 1, memory_order_release);
  while (!atomic_load_explicit(&Release, memory_order_acquire)) {
  }
  return NULL;
}

int main(void) {
  pthread_t Threads[ThreadCount];
  for (unsigned I = 0; I != ThreadCount; ++I) {
    int Result = pthread_create(&Threads[I], NULL, record_stack_id,
                                (void *)(uintptr_t)I);
    assert(Result == 0);
  }
  while (atomic_load_explicit(&Ready, memory_order_acquire) != ThreadCount) {
  }

  int Sequential = 1;
  int OutsideOldPrefix = 0;
  uint64_t IDSpaceBegin = ptsan_id_bits() == 32 ? UINT32_C(0x80000000) : 0;
  for (unsigned I = 1; I != ThreadCount; ++I) {
    Sequential &= IDs[I] == IDs[0] + I * BucketSize;
  }
  for (unsigned I = 0; I != ThreadCount; ++I) {
    OutsideOldPrefix |=
        (uint64_t)IDs[I] >= IDSpaceBegin + 16 * BucketSize;
  }
#ifdef SEQUENTIAL
  assert(Sequential);
#else
  assert(!Sequential);
  assert(OutsideOldPrefix);
#endif

  atomic_store_explicit(&Release, 1, memory_order_release);
  for (unsigned I = 0; I != ThreadCount; ++I) {
    pthread_join(Threads[I], NULL);
  }
  return 0;
}
