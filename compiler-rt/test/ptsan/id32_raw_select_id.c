//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan_plain -fsanitize-ptsan-32-bit-ids -DPTSAN_TEST_32_BIT_IDS -O2 -fno-discard-value-names -mllvm -ptsan-stop-after=late-optimization -S -emit-llvm %s -o %t.ll
// RUN: FileCheck %s < %t.ll
// RUN: %clang_ptsan_id32 -O2 -o %t %s
// RUN: %t
// RUN: not %t left 2> %t.left.err
// RUN: FileCheck %s --check-prefix=BAD < %t.left.err
// RUN: not %t right 2> %t.right.err
// RUN: FileCheck %s --check-prefix=BAD < %t.right.err

#include <assert.h>
#include <stdlib.h>

// The comparison needs raw pointers. GetID must not force the subsequent
// select back to tagged form just to obtain its allocation ID for the check.
__attribute__((noinline))
int read_selected(int *left, int *right, int choose_left, unsigned index) {
  volatile int different = left != right;
  int *selected = choose_left ? left : right;
  return selected[index] + different;
}

// CHECK-LABEL: define {{.*}} @read_selected(
// CHECK-DAG: [[LEFT_ID:%.*]] = call i64 @__ptsan_get_id(ptr %left)
// CHECK-DAG: [[RIGHT_ID:%.*]] = call i64 @__ptsan_get_id(ptr %right)
// CHECK: [[PTR:%.*]] = select i1 [[COND:%.*]], ptr {{%.*}}, ptr {{%.*}}, !ptsan.stripped
// CHECK: [[ID:%.*]] = select i1 [[COND]], i64 [[RIGHT_ID]], i64 [[LEFT_ID]]
// CHECK-NOT: call i64 @__ptsan_get_id(ptr [[PTR]])
// CHECK: call void (...) @__ptsan_access(i64 [[ID]],
// CHECK: ret i32
// BAD: Found bad POINT access

int main(int argc, char **argv) {
  int *left = malloc(2 * sizeof(int));
  int *right = malloc(3 * sizeof(int));
  assert(left && right);
  left[0] = 11;
  left[1] = 17;
  right[0] = 23;
  right[1] = 29;
  right[2] = 31;
  assert(read_selected(left, right, 1, 1) == 18);
  assert(read_selected(left, right, 0, 2) == 32);
  if (argc > 1) {
    int choose_left = argv[1][0] == 'l';
    return read_selected(left, right, choose_left, choose_left ? 2 : 3);
  }
  free(left);
  free(right);
  return 0;
}
