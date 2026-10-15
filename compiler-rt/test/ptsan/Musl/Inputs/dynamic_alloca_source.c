//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// CHECK-NOT: Found bad
// CHECK: dynamic-alloca-source-ok
// CHECK-NOT: Found bad

// Entry-block dynamic allocas receive an ID per invocation. Keep the size
// volatile so this really creates a dynamic alloca. Recursion must preserve
// each caller's bounds and contents, and repeated calls must retire their IDs.
// Mix fixed and dynamic storage so the same batch needs both entry-time and
// deferred bounds writes.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static volatile unsigned count = 37;

__attribute__((noinline)) static void consume(char *p, unsigned size) {
  assert(((uintptr_t)p >> 48) != 0);
  assert(((uintptr_t)p >> 48) != 0xffff);
  p[0] = 3;
  p[size - 1] = 5;
}

__attribute__((noinline)) static void recurse(unsigned depth) {
  char fixed[16];
  unsigned size = count;
  char buffer[size];
  consume(fixed, sizeof(fixed));
  consume(buffer, size);
  assert(((uintptr_t)fixed >> 48) != ((uintptr_t)buffer >> 48));
  if (depth) {
    recurse(depth - 1);
  }
  assert(buffer[0] == 3 && buffer[size - 1] == 5);
  assert(fixed[0] == 3 && fixed[sizeof(fixed) - 1] == 5);
}

// Branching code inlined before an alloca can move it out of the entry block
// without making it repeatable. Keep the branches observable to the optimizer.
// Even a small reachability-search budget must not cause an ID-zero fallback.
__attribute__((always_inline)) static inline void branch(unsigned n) {
  if (n & 1) {
    ++count;
    ++count;
  } else {
    count -= 2;
  }
}

__attribute__((noinline)) static void after_branch(unsigned n) {
  branch(n);
  unsigned size = count;
  char buffer[size];
  consume(buffer, size);
  assert(buffer[0] == 3 && buffer[size - 1] == 5);
}

int main(void) {
  for (unsigned i = 0; i < 1000; ++i) {
    recurse(3);
    after_branch(i);
  }
  puts("dynamic-alloca-source-ok");
  return 0;
}
