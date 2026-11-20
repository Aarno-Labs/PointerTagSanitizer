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

// Buffered stdio must decode ID32 pointers inside libc-created iovecs.
// RUN: %ptsan_musl_id32_nonstrict %s -o %t.id32
// RUN: %run %t.id32 | FileCheck %s
// RUN: %ptsan_musl_id32_strict %s -o %t.id32.strict
// RUN: %run %t.id32.strict | FileCheck %s

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
