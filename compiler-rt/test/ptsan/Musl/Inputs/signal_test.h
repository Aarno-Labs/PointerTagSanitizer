//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Shared assertions for signal-entry regressions. A failure uses write/_exit,
// avoiding stdio or allocator calls from a handler.
#ifndef PTSAN_SIGNAL_TEST_H
#define PTSAN_SIGNAL_TEST_H

#include "../../Inputs/ptsan_test_interface.h"
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#define SIGNAL_CHECK(condition)                                                \
  do {                                                                         \
    if (!(condition)) {                                                        \
      static const char message[] = "signal check failed: " #condition "\n";   \
      (void)write(STDERR_FILENO, message, sizeof(message) - 1);                  \
      _exit(1);                                                                \
    }                                                                          \
  } while (0)

#define SIGNAL_RAW __attribute__((noinline, disable_sanitizer_instrumentation))

extern __thread uint64_t ptsan_next_stack_id;
extern __thread uint64_t ptsan_stack_id_limit;
extern __thread unsigned char ptsan_in_signal_context;
extern volatile uint64_t __ptsan_used_stack_ids;

// Read runtime TLS directly, without requesting an application TLS ID for it.
static SIGNAL_RAW __attribute__((unused)) uint64_t signal_stack_limit(void) {
  return ptsan_stack_id_limit;
}

static SIGNAL_RAW __attribute__((unused)) uint64_t
signal_next_stack_id(void) {
  return ptsan_next_stack_id;
}

static SIGNAL_RAW __attribute__((unused)) unsigned char
signal_in_signal_context(void) {
  return ptsan_in_signal_context;
}

static SIGNAL_RAW __attribute__((unused)) void
signal_set_stack_limit(uint64_t limit) {
  ptsan_stack_id_limit = limit;
}

static SIGNAL_RAW __attribute__((unused)) uint64_t signal_live_stack_ids(void) {
  return __atomic_load_n(&__ptsan_used_stack_ids, __ATOMIC_RELAXED);
}

static inline void signal_check_live(const void *p, size_t size) {
  SIGNAL_CHECK(get_id(p) != 0 && get_id(p) != ptsan_default_id());
#ifdef PTSAN_TEST_32_BIT_IDS
  SIGNAL_CHECK(ptsan_get_base(get_id(p)) == ptsan_get_address(p));
#else
  SIGNAL_CHECK(ptsan_get_base(get_id(p)) == (uintptr_t)p);
#endif
  SIGNAL_CHECK(ptsan_get_size(get_id(p)) == size);
}

// Inspect the saved ID; do not dereference an object after its lifetime ends.
static inline void signal_check_retired(uintptr_t p) {
  SIGNAL_CHECK(get_id(p) != 0 && get_id(p) != ptsan_default_id());
  // Retirement clears the span, not the decode metadata.
  SIGNAL_CHECK(ptsan_get_size(get_id(p)) == 0);
}

static __attribute__((noinline, unused)) void signal_touch(char *p, size_t n) {
  p[0] = 17;
  p[n - 1] = 29;
  __asm__ volatile("" : : "r"(p) : "memory");
}

static inline void signal_check_bytes(const char *p, size_t n) {
  SIGNAL_CHECK(p[0] == 17 && p[n - 1] == 29);
}

#endif
