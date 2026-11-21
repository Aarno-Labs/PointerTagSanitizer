// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// RUN: %ptsan_musl_strict %s -o %t
// RUN: %run %t good
// RUN: %ptsan_musl_id32_strict %s -o %t.id32
// RUN: %run %t.id32 good
// RUN: not %run %t.id32 bad-set 2>&1 | FileCheck %s
// RUN: not %run %t.id32 bad-get 2>&1 | FileCheck %s
// RUN: %ptsan_musl_id32_nonstrict %s -o %t.ns
// RUN: %run %t.ns good
// CHECK: Found bad

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct Filter {
  uint16_t code;
  uint8_t jt, jf;
  uint32_t k;
};
struct Program {
  uint16_t len;
  struct Filter *filter;
};

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }
  int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) {
    return 2;
  }
  struct Filter filter = {6, 0, 0, 0xffffffffU};
  struct Program program = {1, &filter};
  if (setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER, &program, sizeof(program))) {
    return 3;
  }
  int on = 1;
  if (!strcmp(argv[1], "bad-set")) {
    void *p = malloc(sizeof(int) - 1);
    if (!p) {
      return 4;
    }
    return setsockopt(fd, SOL_SOCKET, SO_LOCK_FILTER, p, sizeof(int));
  }
  if (setsockopt(fd, SOL_SOCKET, SO_LOCK_FILTER, &on, sizeof(on))) {
    return 5;
  }
  socklen_t size = sizeof(int);
  if (!strcmp(argv[1], "bad-get")) {
    void *p = malloc(sizeof(int) - 1);
    if (!p) {
      return 6;
    }
    return getsockopt(fd, SOL_SOCKET, SO_LOCK_FILTER, p, &size);
  }
  int locked = 0;
  if (getsockopt(fd, SOL_SOCKET, SO_LOCK_FILTER, &locked, &size) ||
      locked != 1) {
    return 7;
  }
  errno = 0;
  if (setsockopt(fd, SOL_SOCKET, SO_DETACH_FILTER, &on, sizeof(on)) != -1 ||
      errno != EPERM) {
    return 8;
  }
  return close(fd);
}
