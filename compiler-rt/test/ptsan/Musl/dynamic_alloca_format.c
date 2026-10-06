//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %t

// musl's fmt_fp uses an entry-block VLA sized according to the floating-point
// type. Exercise both sizes without depending on the standard FILE pointers.
#include <assert.h>
#include <stdio.h>

int main(void) {
  char buffer[32];
  assert(snprintf(buffer, sizeof(buffer), "%.2f", 1.25) == 4);
  assert(buffer[0] == '1' && buffer[1] == '.' && buffer[2] == '2' &&
         buffer[3] == '5' && buffer[4] == '\0');
  assert(snprintf(buffer, sizeof(buffer), "%.2Lf", 2.75L) == 4);
  assert(buffer[0] == '2' && buffer[1] == '.' && buffer[2] == '7' &&
         buffer[3] == '5' && buffer[4] == '\0');
  return 0;
}
