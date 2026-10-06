//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Unlisted ioctl requests fail closed until their argument layouts are
// explicitly modeled.
//
// XFAIL: *
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -o %t %s
// RUN: %run %t

#include <sys/ioctl.h>

int main(void) {
  const unsigned request = _IO('P', 0x7f);
  (void)ioctl(-1, request, 0);
  return 0;
}
