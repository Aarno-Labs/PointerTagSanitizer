//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: split-file %s %t.dir
// RUN: %clang_ptsan -O2 -mllvm -ptsan-stop-after=discovery -S -emit-llvm \
// RUN:   -o - %t.dir/main.c | FileCheck %s --check-prefix=DISCOVERY
// RUN: %clang_ptsan -O2 -S -emit-llvm -o - %t.dir/main.c | FileCheck %s --check-prefix=IR
// RUN: %clang_ptsan -O2 -mllvm -ptsan-stack-temporal-safety=0 -S -emit-llvm \
// RUN:   -o - %t.dir/main.c | FileCheck %s --check-prefix=IR
// RUN: %clang_ptsan -O2 -mllvm -ptsan-no-stack-id-fast-path -S -emit-llvm \
// RUN:   -o - %t.dir/main.c | FileCheck %s --check-prefix=IR
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error \
// RUN:   %t.dir/main.c %t.dir/helper.c -o %t
// RUN: %t 2>&1 | FileCheck %s --check-prefix=GOOD
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error \
// RUN:   -mllvm -ptsan-stack-temporal-safety=0 \
// RUN:   %t.dir/main.c %t.dir/helper.c -o %t.weak
// RUN: %t.weak 2>&1 | FileCheck %s --check-prefix=GOOD
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error \
// RUN:   -mllvm -ptsan-no-stack-id-fast-path \
// RUN:   %t.dir/main.c %t.dir/helper.c -o %t.slow
// RUN: %t.slow 2>&1 | FileCheck %s --check-prefix=GOOD

// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-abort-on-error \
// RUN:   %t.dir/main.c %t.dir/helper.c -o %t
// RUN: %t 2>&1 | FileCheck %s --check-prefix=GOOD
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-abort-on-error \
// RUN:   -mllvm -ptsan-stack-temporal-safety=0 \
// RUN:   %t.dir/main.c %t.dir/helper.c -o %t.weak
// RUN: %t.weak 2>&1 | FileCheck %s --check-prefix=GOOD
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-abort-on-error \
// RUN:   -mllvm -ptsan-no-stack-id-fast-path \
// RUN:   %t.dir/main.c %t.dir/helper.c -o %t.slow
// RUN: %t.slow 2>&1 | FileCheck %s --check-prefix=GOOD

// Reduced from Fleetbench's protobuf parser: a caller with a stack allocation
// must release its IDs before a musttail call, leaving the call adjacent to its
// return. The separate helper translation unit keeps the local alloca alive.
// Repeated calls also exercise ID allocation/cleanup in the tail callee, with
// temporal safety enabled/disabled and with the stack-ID fast path disabled.

// DISCOVERY-LABEL: define {{.*}} @dispatch(
// DISCOVERY: call void (...) @__ptsan_function_exit()
// DISCOVERY-NEXT: [[RESULT:%.*]] = musttail call i32 @next(
// DISCOVERY-NEXT: ret i32 [[RESULT]]

// IR-LABEL: define {{.*}} @dispatch(
// IR: call void @ptsan_free_stack_ids(
// IR: [[RESULT:%.*]] = musttail call i32 @next(
// IR-NEXT: ret i32 [[RESULT]]

// GOOD-NOT: Found bad
// GOOD: musttail-ok
// GOOD-NOT: Found bad

//--- main.c
#include <assert.h>
#include <stdio.h>

extern void use_local(int *ptr);
extern int next(int value);

__attribute__((noinline)) int dispatch(int value) {
  int local = value;
  use_local(&local);
  [[clang::musttail]] return next(local);
}

int main(void) {
  for (int i = 0; i < 1000; ++i) {
    assert(dispatch(i) == i + 2);
  }
  puts("musttail-ok");
  return 0;
}

//--- helper.c
// Kept separate to prevent elimination of the caller's and callee's allocas.
__attribute__((noinline)) void use_local(int *ptr) {
  ++*ptr;
}

int next(int value) {
  int local = value;
  use_local(&local);
  return local;
}
