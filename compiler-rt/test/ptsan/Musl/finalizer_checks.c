//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t
// RUN: not %run %t bad 2>&1 | FileCheck %s
// RUN: %ptsan_musl_strict -DUSE_ATEXIT -o %t.atexit %s
// RUN: %run %t.atexit
// RUN: not %run %t.atexit bad 2>&1 | FileCheck %s
// CHECK: Found bad

// The trusted finalizer-table walker is uninstrumented, but application
// finalizers must still enforce strict-mode checks. Use a runtime index so
// the invalid access cannot be folded away before instrumentation.
#include <stdlib.h>

static volatile unsigned index;
static volatile char data[4];

#ifndef USE_ATEXIT
__attribute__((destructor))
#endif
static void finalizer(void) {
  data[index] = 'X';
}

int main(int argc, char **argv) {
#ifdef USE_ATEXIT
  if (atexit(finalizer) != 0)
    _Exit(2);
#endif
  index = argc > 1 ? 4 : 0;
  return 0;
}
