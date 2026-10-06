//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error %s -o %t
// RUN: %run %t mmap
// RUN: %run %t 64
// RUN: %run %t allocation
// RUN: not %run %t mmap bad 2>&1 | FileCheck %s
// RUN: not %run %t 64 bad 2>&1 | FileCheck %s
// RUN: not %run %t allocation bad 2>&1 | FileCheck %s
// CHECK: Found bad

// mmap and mmap64 describe pages, whereas malloc describes an exact object.
// Access the last permitted byte and separately the first forbidden byte.
#define _LARGEFILE64_SOURCE
#include "Inputs/ptsan_test_interface.h"
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc < 2) {
    return 1;
  }
  int allocation = argv[1][0] == 'a';
  void *ptr;
  if (allocation) {
    ptr = malloc(20);
  } else if (argv[1][0] == '6') {
    ptr = mmap64(NULL, 20, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  } else {
    ptr = mmap(NULL, 20, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  }
  if (!ptr || ptr == MAP_FAILED) {
    return 2;
  }

  size_t size = allocation ? 20 : (size_t)sysconf(_SC_PAGESIZE);
  uint16_t id = get_id(ptr);
  if (!id || ptsan_get_size(id) != size) {
    return 3;
  }
  volatile char *bytes = ptr;
  bytes[argc > 2 ? size : size - 1] = 42;
  if (allocation) {
    free(ptr);
  } else if (munmap(ptr, 20)) {
    return 4;
  }
  return ptsan_get_base(id) != 0 || ptsan_get_size(id) != 0;
}
