//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Linux abstract UNIX-domain addresses avoid filesystem cleanup while public
// bind and getsockname exercise counted socket-address input and an address
// output paired with an in/out socklen_t.
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t good
// RUN: not %run %t bad-bind 2> %t.bind
// RUN: FileCheck %s < %t.bind
// RUN: not %run %t bad-name 2> %t.name
// RUN: FileCheck %s < %t.name
//
// CHECK: Found bad

#define _GNU_SOURCE
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static socklen_t make_abstract_address(struct sockaddr_un *address) {
  pid_t pid = getpid();
  memset(address, 0, sizeof(*address));
  address->sun_family = AF_UNIX;
  memcpy(address->sun_path + 1, &pid, sizeof(pid));
  return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + sizeof(pid));
}

static int bound_socket(void) {
  int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  struct sockaddr_un *address = malloc(sizeof(*address));
  if (!address) {
    close(fd);
    return -1;
  }
  socklen_t length = make_abstract_address(address);
  int result = bind(fd, (const struct sockaddr *)address, length);
  free(address);
  if (result != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int test_good(void) {
  int fd = bound_socket();
  if (fd < 0)
    return 1;

  struct sockaddr_un *address = malloc(sizeof(*address));
  socklen_t *length = malloc(sizeof(*length));
  if (!address || !length) {
    free(length);
    free(address);
    close(fd);
    return 2;
  }
  *length = sizeof(*address);
  int result = getsockname(fd, (struct sockaddr *)address, length);
  int ok = result == 0 && address->sun_family == AF_UNIX &&
           *length >= offsetof(struct sockaddr_un, sun_path) + 1;
  free(length);
  free(address);
  close(fd);
  return ok ? 0 : 3;
}

static int test_bad_bind(void) {
  int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  char *address = calloc(1, 4);
  if (fd < 0 || !address) {
    free(address);
    if (fd >= 0)
      close(fd);
    return 1;
  }
  (void)bind(fd, (const struct sockaddr *)address, sizeof(struct sockaddr_un));
  free(address);
  close(fd);
  return 0;
}

static int test_bad_name(void) {
  int fd = bound_socket();
  char *address = malloc(4);
  socklen_t length = sizeof(struct sockaddr_un);
  if (fd < 0 || !address) {
    free(address);
    if (fd >= 0)
      close(fd);
    return 1;
  }
  (void)getsockname(fd, (struct sockaddr *)address, &length);
  free(address);
  close(fd);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 1;
  if (!strcmp(argv[1], "good"))
    return test_good();
  if (!strcmp(argv[1], "bad-bind"))
    return test_bad_bind();
  if (!strcmp(argv[1], "bad-name"))
    return test_bad_name();
  return 2;
}
