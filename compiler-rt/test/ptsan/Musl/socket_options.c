//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise common flat socket options through the public musl API.
//
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -o %t %s
// RUN: %run %t good
// RUN: not %run %t bad-set-value 2> %t.set
// RUN: FileCheck %s < %t.set
// RUN: not %run %t bad-get-value 2> %t.get
// RUN: FileCheck %s < %t.get
// RUN: not %run %t bad-get-length 2> %t.length
// RUN: FileCheck %s < %t.length
//
// CHECK: Found bad

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int testGood(void) {
  int sockets[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
    return 1;
  }

  int enabled = 1;
  if (setsockopt(sockets[0], SOL_SOCKET, SO_REUSEADDR, &enabled,
                 sizeof(enabled)) != 0) {
    return 2;
  }

  int socket_type = 0;
  socklen_t size = sizeof(socket_type);
  if (getsockopt(sockets[0], SOL_SOCKET, SO_TYPE, &socket_type, &size) != 0 ||
      socket_type != SOCK_STREAM || size != sizeof(socket_type)) {
    return 3;
  }

  unsigned char partial_type = 0;
  size = sizeof(partial_type);
  if (getsockopt(sockets[0], SOL_SOCKET, SO_TYPE, &partial_type, &size) != 0 ||
      size != sizeof(partial_type)) {
    return 4;
  }

  struct linger linger = {
      .l_onoff = 1,
      .l_linger = 1,
  };
  if (setsockopt(sockets[0], SOL_SOCKET, SO_LINGER, &linger, sizeof(linger)) !=
      0) {
    return 5;
  }

  int tcp_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (tcp_socket < 0) {
    return 6;
  }
  if (setsockopt(tcp_socket, IPPROTO_TCP, TCP_NODELAY, &enabled,
                 sizeof(enabled)) != 0) {
    return 7;
  }

  int ipv6_socket = socket(AF_INET6, SOCK_STREAM, 0);
  if (ipv6_socket >= 0 && setsockopt(ipv6_socket, IPPROTO_IPV6, IPV6_V6ONLY,
                                     &enabled, sizeof(enabled)) != 0) {
    return 8;
  }

  if (ipv6_socket >= 0) {
    close(ipv6_socket);
  }
  close(tcp_socket);
  close(sockets[0]);
  close(sockets[1]);
  return 0;
}

static int openSocket(void) {
  int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (socket_fd < 0) {
    return -1;
  }
  return socket_fd;
}

static int testBadSetValue(void) {
  int socket_fd = openSocket();
  if (socket_fd < 0) {
    return 1;
  }
  char *value = malloc(sizeof(int) - 1);
  if (!value) {
    close(socket_fd);
    return 2;
  }
  (void)setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, value, sizeof(int));
  free(value);
  close(socket_fd);
  return 0;
}

static int testBadGetValue(void) {
  int socket_fd = openSocket();
  if (socket_fd < 0) {
    return 1;
  }
  char *value = malloc(sizeof(int) - 1);
  if (!value) {
    close(socket_fd);
    return 2;
  }
  socklen_t size = sizeof(int);
  (void)getsockopt(socket_fd, SOL_SOCKET, SO_TYPE, value, &size);
  free(value);
  close(socket_fd);
  return 0;
}

static int testBadGetLength(void) {
  int socket_fd = openSocket();
  if (socket_fd < 0) {
    return 1;
  }
  char *size = malloc(sizeof(socklen_t) - 1);
  if (!size) {
    close(socket_fd);
    return 2;
  }
  int value;
  (void)getsockopt(socket_fd, SOL_SOCKET, SO_TYPE, &value, (socklen_t *)size);
  free(size);
  close(socket_fd);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }
  if (!strcmp(argv[1], "good")) {
    return testGood();
  }
  if (!strcmp(argv[1], "bad-set-value")) {
    return testBadSetValue();
  }
  if (!strcmp(argv[1], "bad-get-value")) {
    return testBadGetValue();
  }
  if (!strcmp(argv[1], "bad-get-length")) {
    return testBadGetLength();
  }
  return 3;
}
