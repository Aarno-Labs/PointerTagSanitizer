//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Classic socket filters contain a nested pointer and remain unsupported.
//
// XFAIL: *
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -o %t %s
// RUN: %run %t

#include <sys/socket.h>

int main(void) {
  int filter = 0;
  (void)setsockopt(-1, SOL_SOCKET, SO_ATTACH_FILTER, &filter, sizeof(filter));
  return 0;
}
