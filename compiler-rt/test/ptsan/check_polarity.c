//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan -O2 -mllvm -ptsan-no-opts -mllvm -ptsan-abort-on-error %s -o %t
// RUN: %t
// RUN: not %t raw-under 2>&1 | FileCheck %s --check-prefixes=CHECK,UNDER
// RUN: not %t raw-over 2>&1 | FileCheck %s --check-prefixes=CHECK,OVER
// RUN: not %t tagged-under 2>&1 | FileCheck %s --check-prefixes=CHECK,UNDER
// RUN: not %t tagged-over 2>&1 | FileCheck %s --check-prefixes=CHECK,OVER
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error %s -o %t.opt
// RUN: %t.opt
// RUN: not %t.opt raw-under 2>&1 | FileCheck %s --check-prefixes=CHECK,UNDER
// RUN: not %t.opt raw-over 2>&1 | FileCheck %s --check-prefixes=CHECK,OVER
// RUN: not %t.opt tagged-under 2>&1 | FileCheck %s --check-prefixes=CHECK,UNDER
// RUN: not %t.opt tagged-over 2>&1 | FileCheck %s --check-prefixes=CHECK,OVER
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-no-opts %s -o %t.id32
// RUN: %t.id32
// RUN: not %t.id32 raw-under 2>&1 | FileCheck %s --check-prefixes=CHECK,UNDER
// RUN: not %t.id32 raw-over 2>&1 | FileCheck %s --check-prefixes=CHECK,OVER
// RUN: not %t.id32 tagged-under 2>&1 | FileCheck %s --check-prefixes=CHECK,UNDER
// RUN: not %t.id32 tagged-over 2>&1 | FileCheck %s --check-prefixes=CHECK,OVER
// RUN: %clang_ptsan_id32 -O2 %s -o %t.id32.opt
// RUN: %t.id32.opt
// RUN: not %t.id32.opt raw-under 2>&1 | FileCheck %s --check-prefixes=CHECK,UNDER
// RUN: not %t.id32.opt raw-over 2>&1 | FileCheck %s --check-prefixes=CHECK,OVER
// RUN: not %t.id32.opt tagged-under 2>&1 | FileCheck %s --check-prefixes=CHECK,UNDER
// RUN: not %t.id32.opt tagged-over 2>&1 | FileCheck %s --check-prefixes=CHECK,OVER

// Bounds checks must catch both ends of an object whether min-cut keeps a
// pointer stripped or tagged. A single dynamic-stack access can stay stripped;
// a pointer passed into another function arrives tagged. The accompanying IR
// test checks those choices and their different base calculations explicitly.
// CHECK: Found bad
// UNDER: Min-Base = -1,
// OVER: Min-Base = 16,

#include <stddef.h>
#include <string.h>

__attribute__((noinline)) void store_raw(size_t size, ptrdiff_t index) {
  volatile char buffer[size];
  buffer[index] = 7;
}

__attribute__((noinline)) void store_tagged(volatile char *buffer,
                                          ptrdiff_t index) {
  buffer[index] = 7;
}

int main(int argc, char **argv) {
  char buffer[16];
  store_raw(16, 0);
  store_raw(16, 15);
  store_tagged(buffer, 0);
  store_tagged(buffer, 15);
  if (argc > 1) {
    ptrdiff_t index = strstr(argv[1], "under") ? -1 : 16;
    if (argv[1][0] == 'r') {
      store_raw(16, index);
    } else {
      store_tagged(buffer, index);
    }
  }
  return 0;
}
