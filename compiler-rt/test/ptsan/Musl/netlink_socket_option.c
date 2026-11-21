// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// RUN: %ptsan_musl_strict %s -o %t
// RUN: %run %t good
// RUN: not %run %t bad-set 2>&1 | FileCheck %s
// RUN: %ptsan_musl_id32_strict %s -o %t.id32
// RUN: %run %t.id32 good
// RUN: not %run %t.id32 bad-set 2>&1 | FileCheck %s
// RUN: not %run %t.id32 bad-get 2>&1 | FileCheck %s
// RUN: %ptsan_musl_id32_nonstrict %s -o %t.ns
// RUN: %run %t.ns good
// CHECK: Found bad

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// Linux UAPI constants, kept local because the musl test sysroot has no
// kernel headers (as in socket_filter.c).
enum { NETLINK_ROUTE = 0, NETLINK_BROADCAST_ERROR = 4 };

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }
  int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
  if (fd < 0) {
    return 2;
  }
  int enabled = 1;
  if (!strcmp(argv[1], "bad-set")) {
    void *short_value = malloc(sizeof(int) - 1);
    if (!short_value) {
      return 3;
    }
    return setsockopt(fd, SOL_NETLINK, NETLINK_BROADCAST_ERROR, short_value,
                      sizeof(int));
  }
  if (setsockopt(fd, SOL_NETLINK, NETLINK_BROADCAST_ERROR, &enabled,
                 sizeof(enabled))) {
    return 4;
  }
  socklen_t size = sizeof(int);
  if (!strcmp(argv[1], "bad-get")) {
    void *short_value = malloc(sizeof(int) - 1);
    if (!short_value) {
      return 5;
    }
    return getsockopt(fd, SOL_NETLINK, NETLINK_BROADCAST_ERROR, short_value,
                      &size);
  }
  int value = 0;
  if (getsockopt(fd, SOL_NETLINK, NETLINK_BROADCAST_ERROR, &value, &size) ||
      value != 1 || size != sizeof(value)) {
    return 6;
  }
  errno = 0;
  if (setsockopt(-1, SOL_NETLINK, NETLINK_BROADCAST_ERROR, &enabled,
                 sizeof(enabled)) != -1 ||
      errno != EBADF) {
    return 7;
  }
  return close(fd);
}
