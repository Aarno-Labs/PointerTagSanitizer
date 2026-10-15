//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -O2 \
// RUN:   -mllvm -ptsan-abort-on-error -o %t %s
// RUN: %t 2>&1 | FileCheck %s
// RUN: %ptsan_musl_strict -O2 \
// RUN:   -mllvm -ptsan-abort-on-error -mllvm -ptsan-no-stack-id-fast-path \
// RUN:   -o %t.slow %s
// RUN: %t.slow 2>&1 | FileCheck %s
// CHECK-NOT: Found bad
// CHECK: repeatable-alloca-ok
// CHECK-NOT: Found bad

// Repeated alloca() calls can keep several buffers alive until function exit.
// Each execution needs its own ID, retired when this invocation exits.
// This models oggenc's per-channel buffers and GCC's per-operand buffers.
#include <alloca.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static volatile unsigned count = 4;
static volatile unsigned size = 37;

__attribute__((noinline)) void fill(char *p, unsigned n, char value) {
  p[0] = value;
  p[n - 1] = value;
}

__attribute__((noinline)) void check(const char *p, unsigned n, char value) {
  assert(p[0] == value);
  assert(p[n - 1] == value);
}

__attribute__((noinline)) static void exercise(unsigned depth, unsigned iterations) {
  char *buffers[4];
  unsigned n = size;
  unsigned buffers_count = iterations;
  assert(buffers_count <= 4);
  for (unsigned i = 0; i < buffers_count; ++i) {
    buffers[i] = alloca(n);
    uintptr_t id = (uintptr_t)buffers[i] >> 48;
    assert(id != 0 && id != 0xffff);
    for (unsigned j = 0; j < i; ++j) {
      assert(id != (uintptr_t)buffers[j] >> 48);
    }
    fill(buffers[i], n, (char)(i + 1));
  }
  if (depth != 0) {
    exercise(depth - 1, iterations);
  }
  // All allocations are still live. A later iteration must not overwrite the
  // bounds or contents of an earlier one.
  for (unsigned i = 0; i < buffers_count; ++i) {
    check(buffers[i], n, (char)(i + 1));
  }
}

int main(void) {
  exercise(2, 0); // Empty containers also need cleanup.
  for (unsigned i = 0; i < 20000; ++i) {
    exercise(2, count); // Far more allocations than IDs: cleanup must reclaim.
  }
  puts("repeatable-alloca-ok");
  return 0;
}
