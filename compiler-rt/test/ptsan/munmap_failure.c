//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Failed munmap leaves the mapping live. Preserve its metadata so it remains
// accessible and can subsequently be unmapped successfully.
// RUN: %clang_ptsan -O2 -fno-builtin -o %t %s
// RUN: %run %t
// RUN: %clang_ptsan_id32 -O2 -fno-builtin -o %t.id32 %s
// RUN: %run %t.id32

#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void) {
  size_t size = sysconf(_SC_PAGESIZE);
  volatile char *p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(p != MAP_FAILED);
  PTSanID id = get_id(p);
  assert(id != 0);
  assert(ptsan_get_size(id) == size);

  int result = munmap((void *)p, 0);
  assert(result == -1);
  assert(errno == EINVAL);
  assert(ptsan_get_size(id) == size);
  p[0] = 42;
  assert(p[0] == 42);

  result = munmap((void *)p, size);
  assert(result == 0);
  assert(ptsan_get_size(id) == 0);
  return 0;
}
