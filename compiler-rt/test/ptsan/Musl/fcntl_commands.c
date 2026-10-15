//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise public musl fcntl commands with scalar and fixed-size pointer
// arguments.
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t good
// RUN: not %run %t bad-lock 2> %t.lock
// RUN: FileCheck %s < %t.lock
// RUN: not %run %t bad-owner 2> %t.owner
// RUN: FileCheck %s < %t.owner
// RUN: not %run %t bad-owner-uids 2> %t.owner-uids
// RUN: FileCheck %s < %t.owner-uids
// RUN: not %run %t bad-hint 2> %t.hint
// RUN: FileCheck %s < %t.hint
//
// CHECK: Found bad

#define _GNU_SOURCE

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static int close_with_result(int descriptors[2], int result) {
  if (close(descriptors[0]) != 0 && !result) {
    result = 20;
  }
  if (close(descriptors[1]) != 0 && !result) {
    result = 21;
  }
  return result;
}

static int test_good(void) {
  int descriptors[2];
  if (pipe(descriptors) != 0) {
    return 1;
  }
  const int fd = descriptors[0];

  int descriptor_flags = fcntl(fd, F_GETFD);
  if (descriptor_flags < 0 ||
      fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
    return close_with_result(descriptors, 3);
  }

  struct flock lock;
  memset(&lock, 0, sizeof(struct flock));
  lock.l_type = F_WRLCK;
  lock.l_whence = SEEK_SET;
  (void)fcntl(fd, F_SETLKW, &lock);
  lock.l_type = F_UNLCK;
  (void)fcntl(fd, F_SETLK, &lock);

  lock.l_type = F_WRLCK;
  (void)fcntl(fd, F_GETLK, &lock);
  (void)fcntl(fd, F_OFD_GETLK, &lock);

  struct f_owner_ex owner;
  owner.type = F_OWNER_PID;
  owner.pid = getpid();
  (void)fcntl(fd, F_SETOWN_EX, &owner);
  memset(&owner, 0, sizeof(struct f_owner_ex));
  (void)fcntl(fd, F_GETOWN_EX, &owner);

  uid_t owner_uids[2] = {0, 0};
  (void)fcntl(fd, F_GETOWNER_UIDS, owner_uids);

  uint64_t hint = RWH_WRITE_LIFE_SHORT;
  (void)fcntl(fd, F_SET_RW_HINT, &hint);
  (void)fcntl(fd, F_GET_RW_HINT, &hint);

  return close_with_result(descriptors, 0);
}

static int test_bad(int command, size_t public_size) {
  void *argument = calloc(1, public_size - 1);
  if (!argument) {
    return 1;
  }
  (void)fcntl(-1, command, argument);
  free(argument);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }
  if (!strcmp(argv[1], "good")) {
    return test_good();
  }
  if (!strcmp(argv[1], "bad-lock")) {
    return test_bad(F_GETLK, sizeof(struct flock));
  }
  if (!strcmp(argv[1], "bad-owner")) {
    return test_bad(F_GETOWN_EX, sizeof(struct f_owner_ex));
  }
  if (!strcmp(argv[1], "bad-owner-uids")) {
    return test_bad(F_GETOWNER_UIDS, 2 * sizeof(uid_t));
  }
  if (!strcmp(argv[1], "bad-hint")) {
    return test_bad(F_GET_RW_HINT, sizeof(uint64_t));
  }
  return 2;
}
