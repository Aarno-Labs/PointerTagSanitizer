//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// A decode may be shared across a potentially freeing call, but a later bounds
// check must still observe retirement. Exercise both the live and freed paths.

// RUN: %clang_ptsan_id32 -O2 -fno-discard-value-names \
// RUN:   -mllvm -ptsan-stop-after=late-optimization -S -emit-llvm -o - %s | \
// RUN:   FileCheck %s
// RUN: %clang_ptsan_id32 -O2 -o %t %s
// RUN: %run %t 2> %t.good
// RUN: FileCheck %s --check-prefix=GOOD --allow-empty < %t.good
// RUN: not %t free 2> %t.bad
// RUN: FileCheck %s --check-prefix=BAD < %t.bad

#include <assert.h>
#include <stdlib.h>

static volatile int DoFree;

__attribute__((noinline)) void may_free(char *p) {
  if (DoFree)
    free(p);
}

__attribute__((noinline)) int access_around_call(char *p, long index) {
  char *q = p + index;
  int before = q[0];
  may_free(p);
  return before + q[1];
}

// CHECK-LABEL: define {{.*}} @access_around_call(
// CHECK: call ptr @__ptsan_strip
// CHECK: call void @may_free

// GOOD-NOT: Found bad
// BAD: Found bad POINT access

int main(int argc, char **argv) {
  char *p = malloc(2);
  assert(p);
  p[0] = 3;
  p[1] = 4;

  DoFree = argc > 1;
  int result = access_around_call(p, 0);
  assert(DoFree || result == 7);
  if (!DoFree)
    free(p);
  return 0;
}
