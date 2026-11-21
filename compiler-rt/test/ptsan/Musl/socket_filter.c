//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise classic BPF socket filters, whose program contains a nested
// instruction pointer.
//
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -Wno-alloc-size -o %t %s
// RUN: %run %t good
// RUN: %run %t errors
// RUN: not %run %t bad-filter 2> %t.filter
// RUN: FileCheck %s < %t.filter
//
// CHECK: Found bad

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

struct TestSocketFilter {
  uint16_t code;
  uint8_t jump_true;
  uint8_t jump_false;
  uint32_t value;
};

struct TestSocketFilterProgram {
  uint16_t length;
  struct TestSocketFilter *filter;
};

static int testGood(void) {
  struct TestSocketFilter filter = {0};
  struct TestSocketFilterProgram program = {1, &filter};
  (void)setsockopt(-1, SOL_SOCKET, SO_ATTACH_FILTER, &program,
                   sizeof(program));
  (void)setsockopt(-1, SOL_SOCKET, SO_ATTACH_REUSEPORT_CBPF, &program,
                   sizeof(program));
  return program.filter != &filter;
}

static int testBadFilter(void) {
  struct TestSocketFilterProgram program;
  program.length = 1;
  // Deliberately undersized: the runtime must diagnose the nested range.
  program.filter = malloc(sizeof(struct TestSocketFilter) - 1);
  if (!program.filter) {
    return 1;
  }
  (void)setsockopt(-1, SOL_SOCKET, SO_ATTACH_FILTER, &program,
                   sizeof(program));
  return 0;
}

static int testErrors(void) {
  errno = 0;
  if (setsockopt(-1, SOL_SOCKET, SO_ATTACH_FILTER, NULL,
                 sizeof(struct TestSocketFilterProgram)) != -1 ||
      errno != EBADF) {
    return 1;
  }

  struct TestSocketFilterProgram program = {1, NULL};
  errno = 0;
  if (setsockopt(-1, SOL_SOCKET, SO_ATTACH_FILTER, &program, sizeof(program)) !=
          -1 ||
      errno != EBADF) {
    return 2;
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }
  if (!strcmp(argv[1], "good")) {
    return testGood();
  }
  if (!strcmp(argv[1], "bad-filter")) {
    return testBadFilter();
  }
  if (!strcmp(argv[1], "errors")) {
    return testErrors();
  }
  return 1;
}
