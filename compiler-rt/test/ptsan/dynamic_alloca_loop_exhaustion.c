//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error -o %t %s
// RUN: %t 2>&1 | FileCheck %s --check-prefixes=DEFAULT,STATS
// RUN: %clang_raw -fsanitize=ptsan -O2 -mllvm -ptsan-abort-on-error \
// RUN:   -o %t.normal %s
// RUN: %t.normal 2>&1 | FileCheck %s --check-prefix=DEFAULT
// DEFAULT-NOT: Found bad
// DEFAULT-DAG: repeatable exhaustion and recovery ok
// STATS-DAG: Max used stack IDs at once: 70000
// STATS-DAG: Unreclaimed IDs at exit: 0
// DEFAULT-NOT: Found bad

// Enough live buffers to grow the runtime container and exhaust the ID pool.
// Statistics count default-ID requests too, but they must never be returned to
// the real ID pool.
#include <alloca.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "Inputs/ptsan_test_interface.h"

static volatile unsigned size = 1;
static volatile unsigned count = 70000;
static int saw_default;

__attribute__((noinline)) static void consume(char *p, int expect_real) {
  uintptr_t id = (uintptr_t)p >> 48;
  if (expect_real) {
    assert(id != 0 && id != ptsan_default_id());
  } else if (id == ptsan_default_id()) {
    saw_default = 1;
  }
  *(volatile char *)p = 1;
}

__attribute__((noinline)) static void allocate(unsigned n, int expect_real) {
  for (unsigned i = 0; i < n; ++i) {
    consume(alloca(size), expect_real);
  }
}

int main(void) {
  allocate(count, 0);
  assert(saw_default);
  allocate(4096, 1); // IDs retired on return must be available again.
  puts("repeatable exhaustion and recovery ok");
  return 0;
}
