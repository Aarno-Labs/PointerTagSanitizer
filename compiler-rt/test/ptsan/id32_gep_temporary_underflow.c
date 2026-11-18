//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// A pointer may temporarily precede its object before indexing brings it back
// in range. ID32 conversion must preserve these valid final accesses, both
// with optimized inbounds GEPs and with unoptimized non-inbounds arithmetic.

// RUN: %clang_raw -O2 -S -emit-llvm -o - %s | FileCheck %s --check-prefix=IR
// RUN: %clang_ptsan_id32 -O2 -o %t %s
// RUN: %run %t x
// RUN: %clang_ptsan_id32 -O2 -fno-strict-overflow -mllvm -ptsan-no-opts -o %t.no-opts %s
// RUN: %run %t.no-opts x

// IR: [[DELTA:%.*]] = select i1 {{.*}}, i64 0, i64 -4
// IR: getelementptr inbounds{{( nuw)?}} i8, ptr %p, i64 [[DELTA]]

#include <assert.h>
#include <stdlib.h>

static volatile int TakeInnerLoop = 1;

__attribute__((noinline)) static void
write_through_temporary_underflow(int *p, int adjust) {
  int first = 0;
  int last = 3;
  if (adjust) {
    // The pointer temporarily precedes the allocation, but every access below
    // compensates for that displacement before dereferencing it.
    --p;
    ++first;
    ++last;
  }

  for (int i = first; i < last; ++i) {
    if (TakeInnerLoop) {
      for (++i; i < last + 1; ++i)
        p[i] = 42;
    } else {
      p[i] = 42;
    }
  }
}

int main(int argc, char **argv) {
  int *p = malloc(4 * sizeof(*p));
  assert(p != NULL);

  write_through_temporary_underflow(p, argc > 1);
  assert(p[1] == 42);
  assert(p[2] == 42);
  assert(p[3] == 42);

  free(p);
  return 0;
}
