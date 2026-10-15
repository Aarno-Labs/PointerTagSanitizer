// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: split-file %s %t
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error -c %t/relative.ll -o %t/full.o
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error %t/main.c %t/full.o -o %t/full
// RUN: %t/full
// RUN: not %t/full past-end 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not %t/full before-start 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: %clang_raw -O2 -mllvm -ptsan-strip-ids -c %t/relative.ll -o %t/strip.o
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error %t/main.c %t/strip.o -o %t/strip
// RUN: %t/strip
// BAD: Found bad POINT access

// A relative load must check and strip the address of its four-byte entry,
// while retaining the base pointer's ID in the returned pointer. Both index
// widths and a negative stored displacement are exercised. Strip-only code
// needs the same expansion, but does not add bounds checks.
//--- relative.ll
target triple = "x86_64-unknown-linux-gnu"

define ptr @relative32(ptr %base, i32 %offset) {
  %result = call ptr @llvm.load.relative.i32(ptr %base, i32 %offset)
  ret ptr %result
}

define ptr @relative64(ptr %base, i64 %offset) {
  %result = call ptr @llvm.load.relative.i64(ptr %base, i64 %offset)
  ret ptr %result
}

declare ptr @llvm.load.relative.i32(ptr, i32)
declare ptr @llvm.load.relative.i64(ptr, i64)

//--- main.c
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

extern int32_t *relative32(void *, int32_t);
extern int32_t *relative64(void *, int64_t);

int main(int argc, char **argv) {
  int32_t *table = malloc(2 * sizeof(*table));
  assert(table);
  table[0] = 4;
  table[1] = -4;
  uintptr_t id = (uintptr_t)table >> 48;
  assert(id && id != 0xffff);

  if (argc > 1) {
    // Offset 5 leaves three bytes in the object; the load needs four.
    if (argv[1][0] == 'p')
      relative64(table, 5);
    else
      relative32(table, -4);
    abort(); // A bounds report must happen before control reaches here.
  }

  int32_t *forward = relative32(table, 0);
  int32_t *backward = relative64(table + 1, 0);
  assert(forward == table + 1 && backward == table);
  assert(((uintptr_t)forward >> 48) == id);
  assert(((uintptr_t)backward >> 48) == id);
  assert(*forward == -4 && *backward == 4);
  free(table);
  return 0;
}
