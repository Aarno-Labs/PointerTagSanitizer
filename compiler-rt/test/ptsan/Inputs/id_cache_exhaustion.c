//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise exhaustion and recycling through the ordinary cache callers.
#include "ptsan_test_interface.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>

#define RAW __attribute__((disable_sanitizer_instrumentation, noinline))
extern uint16_t ptsan_get_next_nonstack_id(void *, uint64_t, uint8_t, uint64_t);
extern void ptsan_mark_ptr_freed(void *, uint8_t);
static RAW void retire(char *object, uint16_t id) {
  uintptr_t tagged = (uintptr_t)object | ((uintptr_t)id << 48);
  if (id != ptsan_default_id()) {
    assert(ptsan_get_base(id) == tagged && ptsan_get_size(id) == 1);
  }
  ptsan_mark_ptr_freed((void *)tagged, 1);
  assert(id == ptsan_default_id() || ptsan_get_size(id) == 0);
}

struct Worker {
  unsigned ready, finish;
  uint16_t id;
  char object;
};

static RAW void *worker(void *arg) {
  struct Worker *w = arg;
  w->id = ptsan_get_next_nonstack_id(&w->object, 1, 1, 1);
  assert(w->id && w->id != ptsan_default_id());
  __atomic_store_n(&w->ready, 1, __ATOMIC_RELEASE);
  while (!__atomic_load_n(&w->finish, __ATOMIC_ACQUIRE)) {
    sched_yield();
  }
  retire(&w->object, w->id);
  // Teardown returns the partial allocation and return caches to the pool.
  return 0;
}

// Keep the separate loop-hoisting pass from inserting anchors into this raw
// runtime-ABI test.
RAW __attribute__((optnone)) int main(void) {
  char objects[65536];
  uint16_t ids[65536];
  unsigned char seen[65536] = {0};
  struct Worker w = {0};
  pthread_t thread;

  // Populate the sole thread's cache before pthread_create switches ordinary
  // caches to their smaller multithreaded capacity.
  unsigned count = 0;
  uint16_t id = ptsan_get_next_nonstack_id(&objects[count], 1, 1, 1);
  assert(id && id != ptsan_default_id());
  seen[id] = 1;
  ids[count++] = id;

  assert(pthread_create(&thread, 0, worker, &w) == 0);
  while (!__atomic_load_n(&w.ready, __ATOMIC_ACQUIRE)) {
    sched_yield();
  }
  seen[w.id] = 1;

  for (; count != 65536; ++count) {
    uint16_t id = ptsan_get_next_nonstack_id(&objects[count], 1, 1, 1);
    assert(id);
    if (id == ptsan_default_id()) {
      retire(&objects[count], id);
      break;
    }
    assert(!seen[id]);
    seen[id] = 1;
    ids[count] = id;
  }
  assert(count > 512 && count < 65536);

  // Repeated exhaustion still returns the fallback ID while refills are
  // skipped. Returning IDs below must make allocation resume.
  for (unsigned i = 0; i != 4096; ++i) {
    uint16_t id = ptsan_get_next_nonstack_id(&objects[count], 1, 1, 1);
    assert(id == ptsan_default_id());
    retire(&objects[count], id);
  }

  __atomic_store_n(&w.finish, 1, __ATOMIC_RELEASE);
  assert(pthread_join(thread, 0) == 0);
  seen[w.id] = 0;
  // IDs flushed at thread exit must be available to this thread.
  id = ptsan_get_next_nonstack_id(&objects[count], 1, 1, 1);
  assert(id && id != ptsan_default_id() && !seen[id]);
  seen[id] = 1;
  ids[count++] = id;

  // Pool pressure must publish even a partial return cache.
  --count;
  seen[ids[count]] = 0;
  retire(&objects[count], ids[count]);
  id = ptsan_get_next_nonstack_id(&objects[count], 1, 1, 1);
  assert(id && id != ptsan_default_id() && !seen[id]);
  seen[id] = 1;
  ids[count++] = id;
  while (count) {
    --count;
    retire(&objects[count], ids[count]);
  }
  return 0;
}
