//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// The syscall boundary must diagnose the invalid nested input range before
// entering the kernel.
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: not %run %t 2> %t.err
// RUN: FileCheck %s < %t.err

// CHECK: Found bad

#include <stdlib.h>
#include <sys/uio.h>
#include <unistd.h>

int main(void) {
  int fds[2];
  if (pipe(fds) != 0)
    return 1;

  char *buffer = malloc(4);
  if (!buffer)
    return 2;
  for (int i = 0; i != 4; ++i)
    buffer[i] = 'A' + i;

  struct iovec vector = {buffer, 8};
  (void)writev(fds[1], &vector, 1);
  return 0;
}
