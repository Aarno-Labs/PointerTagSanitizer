//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-reoptimize -mllvm -ptsan-clear-ids-on-cmp=0 -mllvm -ptsan-clear-ids-on-arith=0 -mllvm -ptsan-abort-on-error %s -o %t
// RUN: %run %t
// RUN: not %run %t bad 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-clear-ids-on-cmp=0 -mllvm -ptsan-clear-ids-on-arith=0 -mllvm -ptsan-stop-after=late-optimization -S -emit-llvm %s -o %t.ll
// RUN: FileCheck %s --check-prefix=IR < %t.ll
// RUN: FileCheck %s --check-prefix=GEP < %t.ll

// The check minimum is the loop's cursor PHI, not a GEP. It must not require
// packing on every iteration: the scan can stay raw and retag its result once.
// IR-LABEL: define {{.*}}ptr @find_slot(
// IR: [[RAW:%.*]] = call ptr @__ptsan_strip_id32(ptr %first{{.*}}), {{.*}}!ptsan.stripped
// IR: for.body:
// IR: [[CURSOR:%.*]] = phi ptr {{.*}}[[RAW]]{{.*}}, !ptsan.stripped
// IR-NOT: call ptr @__ptsan_strip_id32(ptr [[CURSOR]]
// IR-NOT: call ptr @__ptsan_tag
// IR: call void (...) @__ptsan_access({{.*}}ptr [[CURSOR]],
// IR: load ptr, ptr [[CURSOR]],
// IR-NOT: call ptr @__ptsan_strip_id32(ptr [[CURSOR]]
// IR-NOT: call ptr @__ptsan_tag
// IR: cleanup.loopexit:
// IR: call ptr @__ptsan_tag_id32(
// GEP-LABEL: define {{.*}}ptr @find_slot(
// GEP: getelementptr {{(inbounds )?}}{{.*}}ptr %cursor.{{.*}}, {{.*}}!ptsan.stripped
// BAD: Found bad

#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stdlib.h>

__attribute__((noinline)) int **find_slot(int **first, long count, int *needle) {
  int **end = first + count;
  for (int **cursor = first; cursor != end; ++cursor) {
    if (*cursor == needle) {
      return cursor;
    }
  }
  return end;
}

int main(int argc, char **argv) {
  int **slots = malloc(32 * sizeof(*slots));
  int *a = malloc(sizeof(*a));
  int *b = malloc(sizeof(*b));
  assert(slots && a && b);
  *a = 17;
  *b = 29;
  for (long i = 0; i < 32; ++i) {
    slots[i] = a;
  }
  slots[23] = b;
  int **found = find_slot(slots, 32, b);
  assert(found == slots + 23);
  assert(get_id(slots) != 0);
  assert(get_id(found) == get_id(slots));
  assert(**found == 29);
  assert(find_slot(slots, 0, b) == slots);
  int **missing = find_slot(slots, 32, NULL);
  assert(missing == slots + 32);
  assert(get_id(missing) == get_id(slots));
  if (argc > 1) {
    int **volatile outside = find_slot(slots, 33, NULL);
    (void)outside;
    assert(0 && "out-of-bounds search must complain");
  }
  free(b);
  free(a);
  free(slots);
}
