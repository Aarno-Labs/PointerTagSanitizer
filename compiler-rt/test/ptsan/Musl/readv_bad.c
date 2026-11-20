//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// The syscall boundary must reject the invalid nested output range;
// returning EFAULT without a PTSan report is not sufficient.
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

  char source[] = "12345678";
  if (write(fds[1], source, sizeof(source) - 1) != sizeof(source) - 1)
    return 2;

  char *buffer = malloc(4);
  if (!buffer)
    return 3;
  struct iovec vector = {buffer, 8};
  (void)readv(fds[0], &vector, 1);
  return 0;
}
