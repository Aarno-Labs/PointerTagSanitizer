//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise musl's public mmap() and syscall() entry points.
// Both receive page-rounded bounds from the syscall handler, even though the
// requested length is smaller. Unmapping that length must retire the ID.
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t mmap-good
// RUN: %run %t syscall-good
// RUN: not %run %t mmap-bad 2> %t.mmap
// RUN: FileCheck %s < %t.mmap
// RUN: not %run %t syscall-bad 2> %t.syscall
// RUN: FileCheck %s < %t.syscall
//
// CHECK: Found bad

#define _GNU_SOURCE
#include "../Inputs/ptsan_test_interface.h"
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

static char *map_with_libc(void) {
  void *result =
      mmap(0, 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return result == MAP_FAILED ? 0 : result;
}

static char *map_with_syscall(void) {
  long result = syscall(SYS_mmap, 0, 4, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return result == -1 ? 0 : (char *)result;
}

static int test_mapping(char *(*create_mapping)(void), int bad, int raw_unmap) {
  volatile char *mapping = create_mapping();
  if (!mapping) {
    return 1;
  }

  size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
  uint16_t id = get_id(mapping);
  if (!id || ptsan_get_size(id) != page_size) {
    return 2;
  }
  mapping[bad ? page_size : page_size - 1] = 'x';
  int result = raw_unmap ? syscall(SYS_munmap, mapping, 4)
                         : munmap((void *)mapping, 4);
  if (result || ptsan_get_size(id) || ptsan_get_base(id)) {
    return 3;
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }
  if (!strcmp(argv[1], "mmap-good")) {
    return test_mapping(map_with_libc, 0, 0);
  }
  if (!strcmp(argv[1], "mmap-bad")) {
    return test_mapping(map_with_libc, 1, 0);
  }
  if (!strcmp(argv[1], "syscall-good")) {
    return test_mapping(map_with_syscall, 0, 1);
  }
  if (!strcmp(argv[1], "syscall-bad")) {
    return test_mapping(map_with_syscall, 1, 1);
  }
  return 3;
}
