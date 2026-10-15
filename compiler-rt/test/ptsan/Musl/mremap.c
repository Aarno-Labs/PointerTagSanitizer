//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise realloc-like metadata transitions through the public mremap() and
// syscall() interfaces.
// Use non-page-sized requests too: bounds and whole-mapping comparisons must
// describe the pages the kernel maps, not the literal byte counts passed in.
//
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -o %t %s
// RUN: %run %t libc-shrink
// RUN: %run %t syscall-shrink
// RUN: %run %t libc-grow
// RUN: %run %t syscall-grow
// RUN: %run %t libc-move
// RUN: %run %t syscall-move
// RUN: %run %t libc-failed
// RUN: %run %t syscall-failed
// RUN: %run %t realloc
//
// RUN: not %run %t libc-stale 2> %t.libc-stale
// RUN: FileCheck %s --check-prefix=STALE < %t.libc-stale
// RUN: not %run %t syscall-stale 2> %t.syscall-stale
// RUN: FileCheck %s --check-prefix=STALE < %t.syscall-stale
//
// STALE: STALE-READY
// STALE: Found bad

#define _GNU_SOURCE

#include "../Inputs/ptsan_test_interface.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

static size_t pageSize(void) {
  long result = sysconf(_SC_PAGESIZE);
  if (result <= 0) {
    return 0;
  }
  return (size_t)result;
}

static void *clearPointerID(const void *pointer) {
  const uintptr_t address_mask = UINT64_C(0x0000ffffffffffff);
  return (void *)((uintptr_t)pointer & address_mask);
}

static void checkLiveBounds(uint16_t id, const void *base, size_t size) {
  assert(id != 0);
  assert(ptsan_get_base(id) == (uint64_t)(uintptr_t)base);
  assert(ptsan_get_size(id) == size);
}

static void checkRetiredBounds(uint16_t id) {
  assert(id != 0);
  assert(ptsan_get_base(id) == 0);
  assert(ptsan_get_size(id) == 0);
}

static char *mapMemory(void *address, size_t size, int additional_flags) {
  void *result = mmap(address, size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | additional_flags, -1, 0);
  if (result == MAP_FAILED) {
    return NULL;
  }
  return (char *)result;
}

static void *remapMemory(bool use_syscall, void *old_address, size_t old_size,
                         size_t new_size, int flags) {
  if (use_syscall) {
    long result =
        syscall(SYS_mremap, old_address, old_size, new_size, flags, NULL);
    if (result == -1) {
      return MAP_FAILED;
    }
    return (void *)result;
  }
  return mremap(old_address, old_size, new_size, flags);
}

__attribute__((noinline)) static char readStaleByte(const volatile char *ptr) {
  return *ptr;
}

static int mapAdjacentPages(size_t page_size, char **mapping, char **blocker) {
  char *reservation = mapMemory(NULL, 2 * page_size, 0);
  if (reservation == NULL) {
    return 1;
  }

  void *base = clearPointerID(reservation);
  if (munmap(reservation, 2 * page_size) != 0) {
    return 2;
  }

  *mapping = mapMemory(base, page_size - 1, MAP_FIXED);
  if (*mapping == NULL) {
    return 3;
  }

  *blocker = mapMemory((char *)base + page_size, page_size, MAP_FIXED);
  if (*blocker == NULL) {
    return 4;
  }
  return 0;
}

static int testGrowth(bool use_syscall) {
  const size_t page_size = pageSize();
  if (page_size == 0) {
    return 1;
  }

  char *reservation = mapMemory(NULL, 2 * page_size, 0);
  if (reservation == NULL) {
    return 2;
  }
  void *base = clearPointerID(reservation);
  if (munmap(reservation, 2 * page_size) != 0) {
    return 3;
  }

  char *mapping = mapMemory(base, page_size - 1, MAP_FIXED);
  if (mapping == NULL) {
    return 4;
  }
  const uint16_t old_id = get_id(mapping);
  checkLiveBounds(old_id, mapping, page_size);
  mapping[0] = 42;

  char *result =
      (char *)remapMemory(use_syscall, mapping, page_size - 1,
                         page_size + 1, 0);
  if (result == MAP_FAILED ||
      clearPointerID(result) != clearPointerID(mapping)) {
    return 5;
  }

  assert(get_id(result) == old_id);
  checkLiveBounds(old_id, result, 2 * page_size);
  assert(result[0] == 42);
  result[2 * page_size - 1] = 1;

  if (munmap(result, page_size + 1) != 0) {
    return 6;
  }
  checkRetiredBounds(old_id);
  return 0;
}

static int testShrink(bool use_syscall) {
  const size_t page_size = pageSize();
  if (page_size == 0) {
    return 1;
  }

  char *mapping = mapMemory(NULL, page_size + 1, 0);
  if (mapping == NULL) {
    return 2;
  }
  const uint16_t old_id = get_id(mapping);
  checkLiveBounds(old_id, mapping, 2 * page_size);

  char *result =
      (char *)remapMemory(use_syscall, mapping, page_size + 1, 1, 0);
  if (result == MAP_FAILED ||
      clearPointerID(result) != clearPointerID(mapping)) {
    return 3;
  }

  assert(get_id(result) == old_id);
  checkLiveBounds(old_id, result, page_size);
  result[page_size - 1] = 1;

  if (munmap(result, 1) != 0) {
    return 4;
  }
  checkRetiredBounds(old_id);
  return 0;
}

static int testFailedGrowth(bool use_syscall) {
  const size_t page_size = pageSize();
  if (page_size == 0) {
    return 1;
  }

  char *mapping;
  char *blocker;
  int setup_result = mapAdjacentPages(page_size, &mapping, &blocker);
  if (setup_result != 0) {
    return 10 + setup_result;
  }

  const uint16_t mapping_id = get_id(mapping);
  checkLiveBounds(mapping_id, mapping, page_size);
  mapping[0] = 42;

  errno = 0;
  void *result = remapMemory(use_syscall, mapping, page_size - 1,
                            page_size + 1, 0);
  if (result != MAP_FAILED || errno != ENOMEM) {
    return 2;
  }

  checkLiveBounds(mapping_id, mapping, page_size);
  assert(mapping[0] == 42);
  if (munmap(mapping, page_size) != 0 || munmap(blocker, page_size) != 0) {
    return 3;
  }
  return 0;
}

static char *moveMapping(bool use_syscall, size_t page_size, char **old_mapping,
                         uint16_t *old_id, char **blocker) {
  if (mapAdjacentPages(page_size, old_mapping, blocker) != 0) {
    return NULL;
  }

  *old_id = get_id(*old_mapping);
  checkLiveBounds(*old_id, *old_mapping, page_size);
  (*old_mapping)[0] = 42;

  char *result = (char *)remapMemory(use_syscall, *old_mapping, page_size - 1,
                                     page_size + 1, MREMAP_MAYMOVE);
  if (result == MAP_FAILED ||
      clearPointerID(result) == clearPointerID(*old_mapping)) {
    return NULL;
  }

  const uint16_t new_id = get_id(result);
  assert(new_id != *old_id);
  checkRetiredBounds(*old_id);
  checkLiveBounds(new_id, result, 2 * page_size);
  assert(result[0] == 42);
  return result;
}

static int testMove(bool use_syscall, bool test_stale_pointer) {
  const size_t page_size = pageSize();
  if (page_size == 0) {
    return 1;
  }

  char *old_mapping;
  char *blocker;
  uint16_t old_id;
  char *result =
      moveMapping(use_syscall, page_size, &old_mapping, &old_id, &blocker);
  if (result == NULL) {
    return 2;
  }

  if (!test_stale_pointer) {
    if (munmap(result, 2 * page_size) != 0 || munmap(blocker, page_size) != 0) {
      return 3;
    }
    return 0;
  }

  char *replacement =
      mapMemory(clearPointerID(old_mapping), page_size, MAP_FIXED);
  if (replacement == NULL) {
    return 4;
  }
  const uint16_t replacement_id = get_id(replacement);
  assert(replacement_id != old_id);
  checkRetiredBounds(old_id);
  checkLiveBounds(replacement_id, replacement, page_size);
  replacement[0] = 7;

  fprintf(stderr, "STALE-READY\n");
  volatile char secret = readStaleByte(old_mapping);
  (void)secret;
  return 5;
}

static int testRealloc(void) {
  const size_t old_size = 1024 * 1024;
  const size_t new_size = 2 * old_size;
  char *allocation = (char *)malloc(old_size);
  if (allocation == NULL) {
    return 1;
  }

  const uint16_t old_id = get_id(allocation);
  void *old_address = clearPointerID(allocation);
  checkLiveBounds(old_id, allocation, old_size);
  allocation[0] = 42;

  char *result = (char *)realloc(allocation, new_size);
  if (result == NULL) {
    return 2;
  }
  assert(result[0] == 42);
  checkLiveBounds(get_id(result), result, new_size);
  if (clearPointerID(result) == old_address) {
    assert(get_id(result) == old_id);
  } else {
    assert(get_id(result) != old_id);
    checkRetiredBounds(old_id);
  }
  free(result);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }
  if (!strcmp(argv[1], "libc-shrink")) {
    return testShrink(false);
  }
  if (!strcmp(argv[1], "syscall-shrink")) {
    return testShrink(true);
  }
  if (!strcmp(argv[1], "libc-grow")) {
    return testGrowth(false);
  }
  if (!strcmp(argv[1], "syscall-grow")) {
    return testGrowth(true);
  }
  if (!strcmp(argv[1], "libc-move")) {
    return testMove(false, false);
  }
  if (!strcmp(argv[1], "syscall-move")) {
    return testMove(true, false);
  }
  if (!strcmp(argv[1], "libc-stale")) {
    return testMove(false, true);
  }
  if (!strcmp(argv[1], "syscall-stale")) {
    return testMove(true, true);
  }
  if (!strcmp(argv[1], "libc-failed")) {
    return testFailedGrowth(false);
  }
  if (!strcmp(argv[1], "syscall-failed")) {
    return testFailedGrowth(true);
  }
  if (!strcmp(argv[1], "realloc")) {
    return testRealloc();
  }
  return 2;
}
