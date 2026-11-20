//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// glibc munmap retires the whole ID after a successful syscall, even for a
// prefix unmap. A rejected request must leave the allocation live.
// This is not a claim of partial-unmap metadata support.
// RUN: %clang_ptsan -O2 -Wall -Wextra -Werror -o %t %s
// RUN: %run %t

// RUN: %clang_ptsan_id32 -O2 -Wall -Wextra -Werror -o %t %s
// RUN: %run %t

#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

static void checkRetired(PTSanID id) {
  assert(id != 0);
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
    PTSanID id = get_id(p);
    assert(id != 0);
    assert(ptsan_get_address((void *)ptsan_get_base(id)) == ptsan_get_address(p));
    assert(ptsan_get_size(id) == size);
    p[page_size] = 42;

    // Retaining a raw address lets us clean up the surviving kernel mapping
    // without using the ID which the interceptor deliberately retires.
    char *raw = (char *)ptsan_get_address(p);
    size_t unmap_size = operation == 0 ? size : operation == 1 ? page_size : 0;
    int result = munmap(p, unmap_size);
    if (operation == 2) {
      assert(result == -1);
      assert(errno == EINVAL);
      assert(ptsan_get_size(id) == size);
      assert(p[page_size] == 42);
      assert(munmap(p, size) == 0);
      checkRetired(id);
    } else {
      assert(result == 0);
      checkRetired(id);
    }

    if (operation == 1) {
      assert(((volatile char *)raw)[page_size] == 42);
      assert(munmap(raw, size) == 0);
    }
  }
  return 0;
}
