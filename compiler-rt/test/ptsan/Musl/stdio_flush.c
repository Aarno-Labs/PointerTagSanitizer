//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// musl flushes a buffered FILE with writev, so its internal iovec contains a
// tagged pointer even though the application does not call writev directly.
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t | FileCheck %s

// CHECK: buffered stdio works

#include <stdio.h>

int main(void) {
  char storage[128];
  if (setvbuf(stdout, storage, _IOFBF, sizeof(storage)) != 0)
    return 1;
  if (fputs("buffered stdio works\n", stdout) == EOF)
    return 2;
  return fflush(stdout) == 0 ? 0 : 3;
}
