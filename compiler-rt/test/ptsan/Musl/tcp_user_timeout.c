//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror %s -o %t
// RUN: %run %t good
// RUN: not %run %t bad-set 2>&1 | FileCheck %s
// RUN: not %run %t bad-get 2>&1 | FileCheck %s
// RUN: not %run %t bad-length 2>&1 | FileCheck %s
// CHECK: Found bad

#include <assert.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv) {
  assert(argc == 2);
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  assert(fd >= 0);

  unsigned timeout = 12345;
  assert(setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &timeout,
                    sizeof(timeout)) == 0);
  unsigned observed = 0;
  socklen_t size = sizeof(observed);
  assert(getsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &observed, &size) == 0);
  assert(size == sizeof(observed));
  assert(observed == timeout);

  if (!strcmp(argv[1], "bad-set")) {
    char *small = calloc(1, sizeof(timeout) - 1);
    assert(small != NULL);
    (void)setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, small, sizeof(timeout));
    assert(0 && "expected a PTSan complaint for the input buffer");
  } else if (!strcmp(argv[1], "bad-get")) {
    char *small = malloc(sizeof(observed) - 1);
    assert(small != NULL);
    (void)getsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, small, &size);
    assert(0 && "expected a PTSan complaint for the output buffer");
  } else if (!strcmp(argv[1], "bad-length")) {
    void *small = malloc(sizeof(socklen_t) - 1);
    assert(small != NULL);
    (void)getsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &observed, small);
    assert(0 && "expected a PTSan complaint for the length pointer");
  } else {
    assert(!strcmp(argv[1], "good"));
  }

  assert(close(fd) == 0);
  return 0;
}
