//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: rm -rf %t && split-file %s %t
// RUN: %opt_ptsan -ptsan-32-bit-ids %t/two_masks.ll -o %t/two_masks.ptsan.ll
// RUN: %clang_raw -c %t/two_masks.ptsan.ll -o %t/two_masks.o
// RUN: %clang_ptsan_id32 -O2 -c %t/main.c -o %t/main.o
// RUN: %clang_ptsan_id32 %t/main.o %t/two_masks.o -o %t/test
// RUN: %t/test one
// RUN: not %t/test two 2> %t/two.err
// RUN: FileCheck %s --check-prefix=BAD < %t/two.err

// A shared strip conversion must load bases for the union of its consumers'
// masks. Reusing the first check's base load leaves lane one poison when the
// second gather activates it. The two-mask case finishes with an intentional
// out-of-bounds access: reaching its PTSan complaint proves that both valid
// gathers completed without an earlier hardware fault.

// BAD: Found bad POINT access

//--- main.c
#include <assert.h>
#include <stdlib.h>

extern int two_masks(int *first, int *second);
extern int one_mask(int *first, int *second);

int main(int argc, char **argv) {
  assert(argc == 2);
  int *first = (int *)malloc(sizeof(int));
  int *second = (int *)malloc(sizeof(int));
  assert(first && second);

  *first = 17;
  *second = 25;
  if (argv[1][0] == 'o')
    assert(one_mask(first, second) == 17);
  else {
    assert(argv[1][0] == 't');
    assert(two_masks(first, second) == 42);
    volatile int out_of_bounds = 1;
    return first[out_of_bounds];
  }

  free(first);
  free(second);
  return 0;
}

//--- two_masks.ll
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare <2 x i32> @llvm.masked.gather.v2i32.v2p0(
    <2 x ptr>, i32 immarg, <2 x i1>, <2 x i32>)

define i32 @one_mask(ptr %first, ptr %second) {
entry:
  %one = insertelement <2 x ptr> poison, ptr %first, i64 0
  %pointers = insertelement <2 x ptr> %one, ptr %second, i64 1
  %values = call <2 x i32> @llvm.masked.gather.v2i32.v2p0(
      <2 x ptr> %pointers, i32 4,
      <2 x i1> <i1 true, i1 false>, <2 x i32> poison)
  %value = extractelement <2 x i32> %values, i64 0
  ret i32 %value
}

define i32 @two_masks(ptr %first, ptr %second) {
entry:
  %one = insertelement <2 x ptr> poison, ptr %first, i64 0
  %pointers = insertelement <2 x ptr> %one, ptr %second, i64 1
  %low = call <2 x i32> @llvm.masked.gather.v2i32.v2p0(
      <2 x ptr> %pointers, i32 4,
      <2 x i1> <i1 true, i1 false>, <2 x i32> poison)
  %high = call <2 x i32> @llvm.masked.gather.v2i32.v2p0(
      <2 x ptr> %pointers, i32 4,
      <2 x i1> <i1 false, i1 true>, <2 x i32> poison)
  %low.value = extractelement <2 x i32> %low, i64 0
  %high.value = extractelement <2 x i32> %high, i64 1
  %sum = add i32 %low.value, %high.value
  ret i32 %sum
}
