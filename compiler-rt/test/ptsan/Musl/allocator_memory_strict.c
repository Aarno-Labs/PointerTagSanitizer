//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -fno-builtin -o %t %s
// RUN: %run %t
// RUN: not %run %t bad 2>&1 | FileCheck %s
// CHECK: Found bad write access: #0. Function ID: 0xdeadbeef

// Exercise the instrumented allocator, including first-use secret setup,
// calloc clearing, and a moving realloc. Application memory operations after
// allocation must also remain checked.
// The allocator internals use normal PTSan with checks disabled, so their
// globals retain IDs when passed to checked helpers such as __unlock.
#include "../Inputs/ptsan_test_interface.h"
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  char *p = malloc(16);
  if (!p || !get_id(p) || get_id(p) == ptsan_default_id()) {
    _Exit(2);
  }
  if (malloc_usable_size(p) != 16) {
    _Exit(8);
  }
  memset(p, 'A', 16);

  char *zero = calloc(4096, 1);
  if (!zero || !get_id(zero) || get_id(zero) == ptsan_default_id()) {
    _Exit(3);
  }
  for (int i = 0; i < 4096; ++i) {
    if (zero[i] != 0) {
      _Exit(4);
    }
  }

  // A tiny slot cannot accommodate this growth in place.
  uintptr_t old_address = (uintptr_t)p & ((UINT64_C(1) << 48) - 1);
  p = realloc(p, 65536);
  if (!p || !get_id(p) || get_id(p) == ptsan_default_id() ||
      ((uintptr_t)p & ((UINT64_C(1) << 48) - 1)) == old_address) {
    _Exit(5);
  }
  for (int i = 0; i < 16; ++i) {
    if (p[i] != 'A') {
      _Exit(6);
    }
  }
  if (malloc_usable_size(p) != 65536) {
    _Exit(9);
  }
  free(p);
  free(zero);

  p = malloc(16);
  if (!p) {
    _Exit(7);
  }
  if (argc > 1) {
    char source[] = "abcdefghijklmnop";
    memcpy(p, source, 17);
  }
  free(p);
  _Exit(0);
}
