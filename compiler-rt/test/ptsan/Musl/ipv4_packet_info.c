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

#define _GNU_SOURCE
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return 2;
  }
  int on = 1;
  if (!strcmp(argv[1], "bad-set")) {
    void *p = malloc(sizeof(int) - 1);
    if (!p) {
      return 3;
    }
    return setsockopt(fd, IPPROTO_IP, IP_PKTINFO, p, sizeof(int));
  }
  if (setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &on, sizeof(on))) {
    return 4;
  }
  int enabled = 0;
  socklen_t size = sizeof(enabled);
  if (!strcmp(argv[1], "bad-get")) {
    void *p = malloc(sizeof(int) - 1);
    if (!p) {
      return 5;
    }
    return getsockopt(fd, IPPROTO_IP, IP_PKTINFO, p, &size);
  }
  if (getsockopt(fd, IPPROTO_IP, IP_PKTINFO, &enabled, &size) || enabled != 1) {
    return 6;
  }
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr))) {
    return 7;
  }
  size = sizeof(addr);
  if (getsockname(fd, (struct sockaddr *)&addr, &size)) {
    return 8;
  }
  if (sendto(fd, "x", 1, 0, (struct sockaddr *)&addr, sizeof(addr)) != 1) {
    return 9;
  }
  char payload;
  struct iovec iov = {&payload, 1};
  union {
    struct cmsghdr align;
    char data[CMSG_SPACE(sizeof(struct in_pktinfo))];
  } control;
  struct msghdr msg = {0};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = control.data;
  msg.msg_controllen = sizeof(control.data);
  if (recvmsg(fd, &msg, MSG_DONTWAIT) != 1 || payload != 'x') {
    return 10;
  }
  struct cmsghdr *header = CMSG_FIRSTHDR(&msg);
  if (!header || header->cmsg_level != IPPROTO_IP ||
      header->cmsg_type != IP_PKTINFO) {
    return 11;
  }
  return close(fd);
}
