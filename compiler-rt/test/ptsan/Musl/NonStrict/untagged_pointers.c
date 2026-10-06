// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_nonstrict -fno-builtin -o %t %s
// RUN: %run %t

// Non-strict mode accepts raw pointers both in the application and in libc.
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  char *buffer = malloc(16);
  assert(buffer);
  char *raw = (char *)((uintptr_t)buffer & UINT64_C(0x0000ffffffffffff));
  raw[0] = 'x';
  raw[1] = '\0';
  assert(strlen(raw) == 1);
  assert(strcmp(raw, "x") == 0);
  free(buffer);
  return 0;
}
