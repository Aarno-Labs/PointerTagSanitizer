//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t | FileCheck %s
// CHECK: Hello, world!

// Exercise ordinary return from main, including musl's exit dispatcher.
#include <stdio.h>

int main(void) {
  printf("Hello, world!\n");
  return 0;
}
