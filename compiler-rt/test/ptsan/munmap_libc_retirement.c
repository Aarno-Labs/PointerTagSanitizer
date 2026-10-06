//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Preserve the existing non-musl interceptor semantics during its move:
// munmap retires the whole ID before the syscall, even for a prefix unmap or
// a rejected request. This is not a claim of partial-unmap metadata support.
// RUN: %clang_ptsan -O2 -Wall -Wextra -Werror -o %t %s
// RUN: %run %t

#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

static void checkRetired(uint16_t id) {
  assert(id != 0);
  assert(ptsan_get_base(id) == 0);
  assert(ptsan_get_size(id) == 0);
}

int main(void) {
  long page_size = sysconf(_SC_PAGESIZE);
  assert(page_size > 0);
  size_t size = (size_t)page_size * 2;

  for (int operation = 0; operation < 3; ++operation) {
    char *p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(p != MAP_FAILED);
    uint16_t id = get_id(p);
    assert(id != 0);
    assert(ptsan_get_base(id) == (uintptr_t)p);
    assert(ptsan_get_size(id) == size);
    p[page_size] = 42;

    // Retaining a raw address lets us clean up the surviving kernel mapping
    // without using the ID which the interceptor deliberately retires.
    char *raw = (char *)((uintptr_t)p & ((UINT64_C(1) << 48) - 1));
    size_t unmap_size = operation == 0 ? size : operation == 1 ? page_size : 0;
    int result = munmap(p, unmap_size);
    if (operation == 2) {
      assert(result == -1);
      assert(errno == EINVAL);
    } else {
      assert(result == 0);
    }
    checkRetired(id);

    if (operation != 0) {
      assert(((volatile char *)raw)[page_size] == 42);
      assert(munmap(raw, size) == 0);
    }
  }
  return 0;
}
