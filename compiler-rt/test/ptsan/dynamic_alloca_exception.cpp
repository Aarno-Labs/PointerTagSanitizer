//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clangxx_ptsan -O2 -mllvm -ptsan-stack-temporal-safety=0 -o %t %s
// RUN: %t 2>&1 | FileCheck %s
// RUN: %clangxx_ptsan -O2 -mllvm -ptsan-stack-temporal-safety=0 \
// RUN:   -mllvm -ptsan-no-stack-id-fast-path -o %t.slow %s
// RUN: %t.slow 2>&1 | FileCheck %s
// CHECK-NOT: Found bad
// CHECK: exception-cleanup-ok
// CHECK-NOT: Found bad
// CHECK: Unreclaimed IDs at exit: 0

#include <alloca.h>
#include <assert.h>
#include <stdio.h>

static volatile unsigned count = 4;
static volatile unsigned size = 37;
static unsigned destructors;

__attribute__((noinline)) static void fill(char *p, unsigned n) {
  p[0] = 17;
  p[n - 1] = 29;
}

__attribute__((noinline)) static void check(const char *p, unsigned n) {
  assert(p[0] == 17);
  assert(p[n - 1] == 29);
}

__attribute__((noinline)) static void throws(bool fail) {
  if (fail) {
    throw 42;
  }
}

// No destructor: these calls initially have no unwind destination in the IR.
__attribute__((noinline)) static void plain(bool fail) {
  for (unsigned i = 0; i < count; ++i) {
    unsigned n = size;
    char *p = (char *)alloca(n);
    fill(p, n);
    throws(fail && i + 1 == count);
    check(p, n);
  }
}

struct Cleanup {
  char buf[32];
  __attribute__((noinline)) ~Cleanup() {
    // Bounds must remain live until user destructors have finished.
    check(buf, sizeof(buf));
    ++destructors;
  }
};

__attribute__((noinline)) static void caught(bool fail) {
  Cleanup cleanup;
  fill(cleanup.buf, sizeof(cleanup.buf));
  unsigned n = size;
  char *p = (char *)alloca(n);
  fill(p, n);
  try {
    plain(fail);
  } catch (int) {
    // A catch inside this frame must not retire this frame's IDs.
    check(p, n);
    throw;
  }
  check(p, n);
}

int main() {
  for (unsigned i = 0; i < 100; ++i) {
    caught(false);
    try {
      caught(true);
      assert(false);
    } catch (int value) {
      assert(value == 42);
    }
  }
  assert(destructors == 200);
  fprintf(stderr, "exception-cleanup-ok\n");
}
