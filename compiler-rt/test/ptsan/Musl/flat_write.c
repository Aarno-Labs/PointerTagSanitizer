//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// A flat pointer reaches PTSan's narrow wrapper for musl's cancellable write
// syscall.  This is the control case for the nested-pointer failures in this
// directory.
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t | FileCheck %s

// CHECK: flat write works

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
  static const char text[] = "flat write works\n";
  char *buffer = malloc(sizeof(text) - 1);
  if (!buffer)
    return 1;

  memcpy(buffer, text, sizeof(text) - 1);
  ssize_t written = write(STDOUT_FILENO, buffer, sizeof(text) - 1);
  free(buffer);
  return written == (ssize_t)(sizeof(text) - 1) ? 0 : 2;
}
