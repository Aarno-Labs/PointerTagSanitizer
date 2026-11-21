// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// RUN: %ptsan_musl_strict %s -o %t
// RUN: %run %t good
// RUN: %ptsan_musl_id32_strict %s -o %t.id32
// RUN: %run %t.id32 good
// RUN: not %run %t.id32 bad-set 2>&1 | FileCheck %s
// RUN: not %run %t.id32 bad-get 2>&1 | FileCheck %s
// RUN: not %run %t.id32 bad-filter 2>&1 | FileCheck %s
// RUN: %ptsan_musl_id32_nonstrict %s -o %t.ns
// RUN: %run %t.ns good
// CHECK: Found bad

#include <errno.h>
#include <netinet/icmp6.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }
  int fd = socket(AF_INET6, SOCK_DGRAM, 0);
  if (fd < 0) {
    return 2;
  }
  int value = 1;
  socklen_t size = sizeof(value);
  if (!strcmp(argv[1], "bad-set") || !strcmp(argv[1], "bad-get")) {
    void *p = malloc(sizeof(int) - 1);
    if (!p) {
      return 3;
    }
    if (!strcmp(argv[1], "bad-set")) {
      return setsockopt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, p, sizeof(int));
    }
    return getsockopt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, p, &size);
  }
  if (!strcmp(argv[1], "bad-filter")) {
    void *p = malloc(sizeof(struct icmp6_filter) - 1);
    if (!p) {
      return 4;
    }
    return setsockopt(-1, IPPROTO_ICMPV6, ICMP6_FILTER, p,
                      sizeof(struct icmp6_filter));
  }
  const int options[] = {IPV6_MULTICAST_HOPS, IPV6_RECVPKTINFO,
                         IPV6_RECVHOPLIMIT};
  for (unsigned i = 0; i < sizeof(options) / sizeof(options[0]); ++i) {
    int result = 0;
    if (setsockopt(fd, IPPROTO_IPV6, options[i], &value, sizeof(value))) {
      return 5;
    }
    size = sizeof(result);
    if (getsockopt(fd, IPPROTO_IPV6, options[i], &result, &size) ||
        result != value) {
      return 6;
    }
  }
  errno = 0;
  value = 6;
  if (setsockopt(-1, IPPROTO_IPV6, IPV6_CHECKSUM, &value, sizeof(value)) !=
          -1 ||
      errno != EBADF) {
    return 7;
  }
  struct icmp6_filter filter;
  ICMP6_FILTER_SETBLOCKALL(&filter);
  errno = 0;
  if (setsockopt(-1, IPPROTO_ICMPV6, ICMP6_FILTER, &filter, sizeof(filter)) !=
          -1 ||
      errno != EBADF) {
    return 8;
  }
  close(fd);
  // Optional privileged integration check; lit itself does not need root.
  if (!strcmp(argv[1], "raw")) {
    fd = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
    if (fd < 0) {
      return 9;
    }
    if (setsockopt(fd, IPPROTO_ICMPV6, ICMP6_FILTER, &filter, sizeof(filter))) {
      return 10;
    }
    struct icmp6_filter output;
    size = sizeof(output);
    if (getsockopt(fd, IPPROTO_ICMPV6, ICMP6_FILTER, &output, &size) ||
        memcmp(&filter, &output, sizeof(filter))) {
      return 11;
    }
    close(fd);
    fd = socket(AF_INET6, SOCK_RAW, IPPROTO_UDP);
    if (fd < 0) {
      return 12;
    }
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_CHECKSUM, &value, sizeof(value))) {
      return 13;
    }
    int offset = 0;
    size = sizeof(offset);
    if (getsockopt(fd, IPPROTO_IPV6, IPV6_CHECKSUM, &offset, &size) ||
        offset != 6) {
      return 14;
    }
    close(fd);
  }
  return 0;
}
