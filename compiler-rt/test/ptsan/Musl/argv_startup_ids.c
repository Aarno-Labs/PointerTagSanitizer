//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t argument ""
// RUN: not %run %t argument "" bad 2>&1 | FileCheck %s

// CHECK: Found bad POINT access
// CHECK-NOT: Found bad

// Argument strings get distinct, permanent IDs in ordinary mode too. Bounds
// include the terminator, even for an empty string. The memory stays writable.
#include "../Inputs/ptsan_test_interface.h"
#include <stdlib.h>

int main(int argc, char **argv) {
  if (argc != 3 && argc != 4)
    _Exit(2);
  unsigned first = get_id(argv[0]);
  unsigned second = get_id(argv[1]);
  unsigned empty = get_id(argv[2]);
  if (!first || !second || !empty || first == ptsan_default_id() ||
      second == ptsan_default_id() || empty == ptsan_default_id() ||
      first == second || first == empty || second == empty)
    _Exit(3);
  if (ptsan_get_size(second) != 9 || ptsan_get_size(empty) != 1)
    _Exit(4);
  volatile char *arg = argv[1];
  volatile char *blank = argv[2];
  if (arg[8] != '\0' || blank[0] != '\0')
    _Exit(5);
  arg[0] = 'A';
  if (arg[0] != 'A')
    _Exit(6);
  if (argc == 4) {
    (void)arg[9];
    _Exit(7);
  }
  _Exit(0);
}
