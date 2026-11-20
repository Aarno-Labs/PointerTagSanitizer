//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Link with the matching instrumented musl/libc++/libc++abi/libunwind stack.
// The musl C-only lit configuration does not supply these C++ libraries.
#include "ptsan_test_interface.h"
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>

struct Payload {
  int *pointer;
};

struct Guard {
  int *count;
  int *pointer;
  __attribute__((noinline)) ~Guard() {
    assert(*pointer == 42);
    ++*count;
  }
};

__attribute__((noinline)) static void raise(int *pointer, int *count) {
  Guard guard{count, pointer};
  throw Payload{pointer};
}

__attribute__((noinline)) static void rethrow(int *pointer, int *count) {
  Guard guard{count, pointer};
  try {
    raise(pointer, count);
  } catch (const Payload &payload) {
    assert(payload.pointer == pointer);
    assert(*payload.pointer == 42);
    throw;
  }
}

__attribute__((noinline)) static void exercise(bool bad) {
  int *pointer = new int(42);
  int destroyed = 0;
  assert(get_id(pointer) != 0);
#ifdef HIGH_IDS
  assert((get_id(pointer) & UINT32_C(0x80000000)) != 0);
#endif
#if defined(__x86_64__)
  // Force a live encoded pointer through a callee-saved machine register.
  register int *saved asm("r12") = pointer;
  asm volatile("" : "+r"(saved));
#else
  int *saved = pointer;
#endif
  try {
    rethrow(pointer, &destroyed);
    abort();
  } catch (const Payload &payload) {
#if defined(__x86_64__)
    asm volatile("" : "+r"(saved));
#endif
    assert(destroyed == 2);
    assert(saved == pointer);
    assert(payload.pointer == pointer);
    assert(get_id(saved) == get_id(pointer));
    assert(*saved == 42);
    if (bad) {
      volatile int *escaped = payload.pointer;
      escaped[1] = 99;
      abort();
    }
  }
  delete pointer;
}

static void *worker(void *) {
  exercise(false);
  return nullptr;
}

int main(int argc, char **) {
  exercise(argc > 1);
  pthread_t thread;
  assert(pthread_create(&thread, nullptr, worker, nullptr) == 0);
  assert(pthread_join(thread, nullptr) == 0);
  return 0;
}
