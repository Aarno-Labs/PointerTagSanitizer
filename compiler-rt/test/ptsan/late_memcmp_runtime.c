//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: split-file %s %t.dir
// RUN: %clang_raw -O2 -c %t.dir/raw.c -o %t.raw.o
// RUN: %clang_raw -O2 -I%S  -Xclang -disable-llvm-passes -S -emit-llvm %t.dir/main.c -o %t.input.ll
// RUN: %opt_ptsan -ptsan-reoptimize -ptsan-runtime-validate %t.input.ll -o %t.ll
// RUN: FileCheck %s < %t.ll
// RUN: %clang_ptsan -O2 -mllvm -ptsan-disable %t.ll %t.raw.o -o %t
// RUN: %run %t

// RUN: %clang_raw -O2 -I%S -DPTSAN_TEST_32_BIT_IDS -Xclang -disable-llvm-passes -S -emit-llvm %t.dir/main.c -o %t.input.ll
// RUN: %opt_ptsan -ptsan-32-bit-ids -ptsan-reoptimize -ptsan-runtime-validate %t.input.ll -o %t.ll
// RUN: FileCheck %s < %t.ll
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-disable %t.ll %t.raw.o -o %t
// RUN: %run %t

// Keep memcmp until instrumentation so this exercises late libcall expansion,
// rather than frontend expansion of the call into checked loads.
// Compare tagged buffers with each other and with a raw buffer to verify that
// backend lowering preserves the pointer handling needed by the checked call.
// CHECK-LABEL: define {{.*}}@compare(
// CHECK: call i32 @memcmp({{.*}}i64 {{.*}}16)

//--- raw.c
unsigned char *raw_buffer(void) {
  static unsigned char buffer[16];
  for (int i = 0; i < 16; ++i) {
    buffer[i] = 7;
  }
  return buffer;
}

//--- main.c
#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

unsigned char *raw_buffer(void);

__attribute__((noinline)) int compare(const void *a, const void *b) {
  return memcmp(a, b, 16);
}

int main(void) {
  unsigned char *a = malloc(16);
  unsigned char *b = malloc(16);
  assert(a != NULL && b != NULL);
  assert(get_id(a) != 0 && get_id(b) != 0);
  memset(a, 7, 16);
  memset(b, 7, 16);
  assert(compare(a, b) == 0);
  b[15] = 8;
  assert(compare(a, b) < 0);
  assert(compare(b, a) > 0);

  // The same calls must also accept pointers received without an ID.
  unsigned char *raw_a = raw_buffer();
  assert(get_id(raw_a) == 0);
  assert(compare(raw_a, raw_a) == 0);
  assert(compare(raw_a, b) < 0);
  assert(compare(b, raw_a) > 0);
  free(b);
  free(a);
  return 0;
}
