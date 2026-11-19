//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// strip-ptrtoint takes precedence over arithmetic stripping: once a conversion
// produces a raw integer address, subtraction must not try to decode it again.
// Function-scoped stripping must also work through the full optimization pipeline
// when the module-wide arithmetic policy is off.

// RUN: split-file %s %t.dir
// RUN: %clang_ptsan -O2 -mllvm -ptsan-strip-ptrtoint -mllvm -ptsan-clear-ids-on-arith=1 %t.dir/helper.c %t.dir/main.c -o %t
// RUN: %run %t
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-clear-ids-on-arith=0 -mllvm -ptsan-clear-ids-on-arith-function=difference -mllvm -ptsan-strip-ptrtoint-function=address %t.dir/helper.c %t.dir/main.c -o %t.functions
// RUN: %run %t.functions
// RUN: %clang_ptsan_plain -O2 -mllvm -ptsan-strip-ptrtoint -mllvm -ptsan-clear-ids-on-arith=1 -mllvm -ptsan-stop-after=discovery -S -emit-llvm %t.dir/helper.c -o %t.ll
// RUN: FileCheck %s < %t.ll

// CHECK-LABEL: define {{.*}} @difference(
// CHECK-NOT: @__ptsan_strip_i64
// CHECK: ret i64

//--- helper.c
#include <stdint.h>

uintptr_t difference(const void *left, const void *right) {
  return (uintptr_t)left - (uintptr_t)right;
}

uintptr_t address(const void *pointer) {
  return (uintptr_t)pointer;
}

//--- main.c
#include <assert.h>
#include <stdint.h>

uintptr_t difference(const void *, const void *);
uintptr_t address(const void *);

static char global;

int main(void) {
  char local;
  // Separate translation units keep the reference subtraction on integer
  // return values. Stack and global addresses expose a second ID32 decode
  // that would otherwise be hidden when both addresses have the same high bits.
  uintptr_t expected = address(&local) - address(&global);
  assert(difference(&local, &global) == expected);
  assert(difference(&global, &local) == (uintptr_t)0 - expected);
  return 0;
}
