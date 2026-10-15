//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict %s -o %t
// RUN: echo input | %t 2>&1 | FileCheck %s
// CHECK-NOT: Found bad
// CHECK: stdout-ok
// CHECK-NEXT: stderr-ok
// CHECK-NOT: Found bad

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
  // These are constant pointer globals, not constant FILE objects. Their
  // stored values must be tagged before the instrumented stdio code runs.
  assert((uintptr_t)stdin >> 48);
  assert((uintptr_t)stdout >> 48);
  assert((uintptr_t)stderr >> 48);
  char buffer[16];
  assert(fgets(buffer, sizeof buffer, stdin) == buffer);
  assert(buffer[0] == 'i' && buffer[4] == 't' && buffer[5] == '\n');
  assert(fwrite("stdout-ok\n", 1, 10, stdout) == 10);
  assert(fflush(stdout) == 0);
  assert(fprintf(stderr, "%s\n", "stderr-ok") == 10);
  return 0;
}
