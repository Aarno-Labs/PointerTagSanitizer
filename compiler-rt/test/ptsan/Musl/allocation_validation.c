//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -fno-builtin -o %t %s
// RUN: not %run %t realloc interior 2>&1 | FileCheck %s --check-prefix=INTERIOR
// RUN: not %run %t usable interior 2>&1 | FileCheck %s --check-prefix=INTERIOR
// RUN: not %run %t realloc freed 2>&1 | FileCheck %s --check-prefix=FREED
// RUN: not %run %t usable freed 2>&1 | FileCheck %s --check-prefix=FREED
// INTERIOR: PTsan found invalid (maybe double, maybe something else?) free!
// FREED: PTSan found double-free!

// Both entry points must validate the allocation before entering musl's
// non-checking allocator internals. They share free's validation diagnostics.
#include <malloc.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  if (argc != 3) {
    return 1;
  }
  char *ptr = malloc(32);
  if (!ptr) {
    _Exit(2);
  }
  if (argv[2][0] == 'f') {
    free(ptr);
  } else {
    ++ptr;
  }
  if (argv[1][0] == 'r') {
    void *volatile result = realloc(ptr, 64);
    (void)result;
  } else {
    volatile size_t size = malloc_usable_size(ptr);
    (void)size;
  }
  _Exit(0);
}
