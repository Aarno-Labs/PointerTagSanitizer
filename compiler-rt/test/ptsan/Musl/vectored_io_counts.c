//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Vector-count validation must preserve the public Linux API behavior without
// treating an untouched iovec pointer as a PTSan memory error.
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t

#include <errno.h>
#include <stdlib.h>
#include <sys/uio.h>
#include <unistd.h>

int main(void) {
  int fds[2];
  if (pipe(fds))
    return 1;

  char *allocation = malloc(1);
  if (!allocation)
    return 2;
  struct iovec *invalid = (struct iovec *)(allocation + 1);

  if (readv(fds[0], invalid, 0) != 0)
    return 3;

  errno = 0;
  if (writev(fds[1], invalid, UIO_MAXIOV + 1) != -1)
    return 4;
  if (errno != EINVAL)
    return 5;

  free(allocation);
  close(fds[0]);
  close(fds[1]);
  return 0;
}
