//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// TCP_ZEROCOPY_RECEIVE contains nested addresses and is not handled by PTSan.
// Even an invalid-fd probe should reach the kernel and return EBADF, rather
// than aborting in the interceptor. Keep this separate from flat-option tests.
// XFAIL: *
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror %s -o %t
// RUN: %run %t

#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

int main(void) {
  int value = 0;
  socklen_t size = sizeof(value);
  assert(getsockopt(-1, IPPROTO_TCP, TCP_ZEROCOPY_RECEIVE, &value, &size) == -1);
  assert(errno == EBADF);
  return 0;
}
