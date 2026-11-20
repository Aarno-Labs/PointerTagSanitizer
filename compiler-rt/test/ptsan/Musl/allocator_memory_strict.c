//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -fno-builtin -o %t %s
// RUN: %run %t
// RUN: not %run %t bad 2>&1 | FileCheck %s

// The same allocator boundary preserves IDs through realloc/free in every mode.
// RUN: %ptsan_musl_nonstrict -fno-builtin %s -o %t.nonstrict
// RUN: %run %t.nonstrict
// RUN: not %run %t.nonstrict bad 2>&1 | FileCheck %s

// Check calloc, moving realloc, and checked memory calls with translated pointers.
// RUN: %ptsan_musl_id32_nonstrict -fno-builtin %s -o %t.id32
// RUN: %run %t.id32
// RUN: not %run %t.id32 bad 2>&1 | FileCheck %s
// RUN: %ptsan_musl_id32_strict -fno-builtin %s -o %t.id32.strict
// RUN: %run %t.id32.strict
// RUN: not %run %t.id32.strict bad 2>&1 | FileCheck %s
// RUN: %ptsan_musl_id32_strict_indicator -fno-builtin %s -o %t.id32.indicator
// RUN: %run %t.id32.indicator
// RUN: not %run %t.id32.indicator bad 2>&1 | FileCheck %s
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
  uintptr_t old_address = ptsan_get_address(p);
  p = realloc(p, 65536);
  if (!p || !get_id(p) || get_id(p) == ptsan_default_id() ||
      ptsan_get_address(p) == old_address) {
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
