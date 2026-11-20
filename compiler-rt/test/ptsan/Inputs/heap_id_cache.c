//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Shared by the host and instrumented-musl tests.
#include "ptsan_test_interface.h"
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>

static void check_live(char *p, size_t size) {
  assert(p && get_id(p) != 0 && get_id(p) != ptsan_default_id());
#ifdef PTSAN_TEST_32_BIT_IDS
  assert(ptsan_get_base(get_id(p)) == ptsan_get_address(p));
#else
  assert(ptsan_get_base(get_id(p)) == (uintptr_t)p);
#endif
  assert(ptsan_get_size(get_id(p)) == size);
  p[0] = 17;
  p[size - 1] = 29;
}

static void release(char *p) {
  PTSanID id = get_id(p);
  assert(p[0] == 17 && p[42] == 29);
  free(p);
  assert(ptsan_get_size(id) == 0);
}

static void burst(unsigned count) {
  char *p[513];
  for (unsigned i = 0; i != count; ++i) {
    p[i] = malloc(43);
  }
  // Check all simultaneously live IDs, including across cache boundaries.
  for (unsigned i = 0; i != count; ++i) {
    check_live(p[i], 43);
  }
  for (unsigned i = 0; i != count; ++i) {
    release(p[i]);
  }
}

static void *free_elsewhere(void *arg) {
  char **p = arg;
  for (unsigned i = 0; i != 257; ++i) {
    release(p[i]);
  }
  burst(257);
  return 0;
}

static void *short_lived(void *arg) {
  (void)arg;
  // Leave both unused allocation IDs and returned IDs in the exiting thread.
  burst(33);
  // The consumed allocation slots still contain stale IDs. Teardown must not
  // return this live object's ID along with the unused part of the batch.
  char *p = malloc(43);
  check_live(p, 43);
  return p;
}

int main(void) {
  static const unsigned sizes[] = {255, 256, 257, 511, 512, 513};
  for (unsigned round = 0; round != 1024; ++round) {
    burst(sizes[round % 6]);
  }

  for (unsigned round = 0; round != 16; ++round) {
    char *p[514];
    for (unsigned i = 0; i != 514; ++i) {
      p[i] = malloc(43);
      check_live(p[i], 43);
    }
    pthread_t a, b;
    assert(pthread_create(&a, 0, free_elsewhere, p) == 0);
    assert(pthread_create(&b, 0, free_elsewhere, p + 257) == 0);
    assert(pthread_join(a, 0) == 0);
    assert(pthread_join(b, 0) == 0);
  }

  // Enough fresh threads to exhaust the 16-bit pool if either cache leaks on
  // teardown.
  for (unsigned round = 0; round != 2048; ++round) {
    pthread_t thread;
    void *result;
    assert(pthread_create(&thread, 0, short_lived, 0) == 0);
    assert(pthread_join(thread, &result) == 0);
    burst(513);
    check_live(result, 43);
    release(result);
  }
  burst(513);
  return 0;
}
