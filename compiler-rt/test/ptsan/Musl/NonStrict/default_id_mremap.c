//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// After ID exhaustion, mremap must preserve the default ID so that a later
// munmap (including one inside musl free) still accepts the mapping.
//
// RUN: %ptsan_musl_nonstrict -Wall -Wextra -Werror -fno-builtin \
// RUN:   -o %t %s
// RUN: %run %t

#define _GNU_SOURCE

#include "../../Inputs/ptsan_test_interface.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

enum { AllocationCount = 70000 };

static void *rawPointer(const void *pointer) {
  return (void *)((uintptr_t)pointer & UINT64_C(0x0000ffffffffffff));
}

static void *remap(bool use_syscall, void *address, size_t old_size,
                   size_t new_size, int flags) {
  if (use_syscall)
    return (void *)syscall(SYS_mremap, address, old_size, new_size, flags,
                           NULL);
  return mremap(address, old_size, new_size, flags);
}

static void testMapping(bool use_syscall, size_t page_size) {
  char *mapping = mmap(NULL, 3 * page_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(mapping != MAP_FAILED);
  assert(get_id(mapping) == ptsan_default_id());
  mapping[0] = 42;

  // Untracked mappings, used by musl's stack discovery, must remain untagged.
  void *raw = rawPointer(mapping);
  void *result = remap(use_syscall, raw, 3 * page_size, 3 * page_size, 0);
  assert(result == raw);
  assert(get_id(result) == 0);
  errno = 0;
  result = remap(use_syscall, (char *)raw + 1, 3 * page_size, page_size, 0);
  assert(result == MAP_FAILED && errno == EINVAL);

  // A successful in-place shrink must retain the fallback ID.
  char *shrunk = remap(use_syscall, mapping, 3 * page_size, 2 * page_size, 0);
  assert(shrunk != MAP_FAILED);
  assert(rawPointer(shrunk) == raw);
  assert(get_id(shrunk) == ptsan_default_id());
  assert(shrunk[0] == 42);

  // Occupy the released page to make growth fail without MAYMOVE and force
  // relocation with it. The address was reserved by our original mapping.
  void *blocker = mmap((char *)raw + 2 * page_size, page_size, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  assert(blocker != MAP_FAILED);
  errno = 0;
  result = remap(use_syscall, shrunk, 2 * page_size, 3 * page_size, 0);
  assert(result == MAP_FAILED && errno == ENOMEM);
  assert(shrunk[0] == 42);

  char *moved =
      remap(use_syscall, shrunk, 2 * page_size, 3 * page_size, MREMAP_MAYMOVE);
  assert(moved != MAP_FAILED);
  assert(rawPointer(moved) != raw);
  assert(get_id(moved) == ptsan_default_id());
  assert(moved[0] == 42);
  moved[3 * page_size - 1] = 17;
  assert(munmap(moved, 3 * page_size) == 0);
  assert(munmap(blocker, page_size) == 0);
}

int main(void) {
  void **live = calloc(AllocationCount, sizeof(*live));
  assert(live);
  for (size_t i = 0; i < AllocationCount; ++i) {
    live[i] = malloc(32);
    assert(live[i]);
  }
  assert(get_id(live[AllocationCount - 1]) == ptsan_default_id());

  long page_size = sysconf(_SC_PAGESIZE);
  assert(page_size > 0);
  testMapping(false, (size_t)page_size);
  testMapping(true, (size_t)page_size);

  // Exercise the allocator path: mallocng realloc remaps large allocations,
  // saves the result in its metadata, and passes it to munmap during free.
  char *large = malloc(256 * 1024);
  assert(large);
  large[0] = 42;
  char *resized = realloc(large, 512 * 1024);
  assert(resized);
  assert(get_id(resized) == ptsan_default_id());
  assert(resized[0] == 42);
  resized[512 * 1024 - 1] = 17;
  free(resized);

  for (size_t i = 0; i < AllocationCount; ++i)
    free(live[i]);
  free(live);
  return 0;
}
