//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -DSTRICT -o %t %s
// RUN: %run %t
// RUN: not %run %t bad 2>&1 | FileCheck %s
// CHECK: Found bad

#include "../Inputs/ptsan_test_interface.h"
#include <stdlib.h>
#include <sys/auxv.h>

static size_t checkString(unsigned long key) {
  volatile char *str = (void *)getauxval(key);
  unsigned id = get_id(str);
  if (!id || id == ptsan_default_id())
    _Exit(2);
  size_t length = 0;
  while (str[length])
    ++length;
  if (ptsan_get_size(id) != length + 1)
    _Exit(3);
  return length;
}

int main(int argc, char **argv) {
  size_t length = checkString(AT_EXECFN);
  checkString(AT_PLATFORM);
#ifndef STRICT
  // AT_BASE_PLATFORM is optional on x86-64. The missing-key getauxval path
  // touches errno, whose TLS storage has a separate strict-mode limitation.
  if (getauxval(AT_BASE_PLATFORM))
    checkString(AT_BASE_PLATFORM);
#endif
  if (argc > 1) {
    volatile char *str = (void *)getauxval(AT_EXECFN);
    (void)str[length + 1];
  }
  _Exit(0);
}
