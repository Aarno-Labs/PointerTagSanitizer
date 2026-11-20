//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Reassociation hides a ptrtoint from subtraction stripping. Truncation to int
// discards the remaining high ID bits in 16-bit mode, but not a 32-bit origin.
// RUN: split-file %s %t.dir
// RUN: %clang_ptsan -O2 -I%S %t.dir/remaining.c %t.dir/main.c -o %t
// RUN: %run %t
// RUN: %clang_ptsan -O2 -mllvm -ptsan-runtime-validate-arith -I%S %t.dir/remaining.c %t.dir/main.c -o %t.validate
// RUN: %run %t.validate

//--- remaining.c
#include <stddef.h>

// mallocng's in-place realloc computes this before clearing the unused tail.
// LLVM reassociates it to end - (p + n), hiding one ptrtoint behind an add.
int remaining(char *end, char *p, size_t n) {
  return end - p - n;
}

//--- main.c
#include <assert.h>
#include <stdlib.h>

#include "Inputs/ptsan_test_interface.h"

int remaining(char *end, char *p, size_t n);

int main(void) {
  char *p = malloc(512);
  assert(p);
  assert(get_id(p) != 0);
  char *raw = (char *)(uintptr_t)ptsan_get_address(p);

  assert(remaining(p + 256, raw, 128) == 128);
  assert(remaining(raw + 256, p, 128) == 128);
  assert(remaining(p + 256, p, 128) == 128);
  free(p);
  return 0;
}
