//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Deferred public partial, tagless, repeated, and cross-mapping munmap support.
// These operations should eventually succeed and update metadata precisely.
//
// XFAIL: *
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -o %t %s
// RUN: %run %t libc-prefix
// RUN: %run %t libc-suffix
// RUN: %run %t libc-middle
// RUN: %run %t libc-tagless
// RUN: %run %t libc-repeated
// RUN: %run %t libc-cross

#define _GNU_SOURCE

#include "../Inputs/ptsan_test_interface.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static size_t pageSize(void) {
  long result = sysconf(_SC_PAGESIZE);
  if (result <= 0) {
    return 0;
  }
  return (size_t)result;
}

static char *clearPointerID(char *pointer) {
  const uintptr_t address_mask = UINT64_C(0x0000ffffffffffff);
  return (char *)((uintptr_t)pointer & address_mask);
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

static int unmapMemory(void *address, size_t size) {
  return munmap(address, size);
}

static int testSimpleUnsupportedRange(const char *shape) {
  const size_t page_size = pageSize();
  if (page_size == 0) {
    return 1;
  }

  const size_t mapping_size = 4 * page_size;
  char *mapping = mapMemory(NULL, mapping_size, 0);
  if (mapping == NULL) {
    return 2;
  }
  const uint16_t mapping_id = get_id(mapping);
  checkLiveBounds(mapping_id, mapping, mapping_size);

  void *unmap_address = mapping;
  size_t unmap_size = mapping_size;
  if (!strcmp(shape, "prefix")) {
    unmap_size = page_size;
  } else if (!strcmp(shape, "suffix")) {
    unmap_address = mapping + 3 * page_size;
    unmap_size = page_size;
  } else if (!strcmp(shape, "middle")) {
    unmap_address = mapping + page_size;
    unmap_size = 2 * page_size;
  } else if (!strcmp(shape, "tagless")) {
    unmap_address = clearPointerID(mapping);
  } else {
    return 3;
  }

  if (unmapMemory(unmap_address, unmap_size) != 0) {
    return 4;
  }

  if (!strcmp(shape, "prefix")) {
    checkLiveBounds(mapping_id, mapping + page_size, 3 * page_size);
  } else if (!strcmp(shape, "suffix")) {
    checkLiveBounds(mapping_id, mapping, 3 * page_size);
  } else {
    checkRetiredBounds(mapping_id);
  }
  return 0;
}

static int testRepeatedUnmap(void) {
  const size_t page_size = pageSize();
  if (page_size == 0) {
    return 1;
  }

  char *mapping = mapMemory(NULL, page_size, 0);
  if (mapping == NULL) {
    return 2;
  }
  const uint16_t mapping_id = get_id(mapping);
  checkLiveBounds(mapping_id, mapping, page_size);

  if (unmapMemory(mapping, page_size) != 0) {
    return 3;
  }
  checkRetiredBounds(mapping_id);

  if (unmapMemory(mapping, page_size) != 0) {
    return 4;
  }
  checkRetiredBounds(mapping_id);
  return 0;
}

static int testCrossMappingUnmap(void) {
  const size_t page_size = pageSize();
  if (page_size == 0) {
    return 1;
  }

  const size_t range_size = 2 * page_size;
  char *reservation = mapMemory(NULL, range_size, 0);
  if (reservation == NULL) {
    return 2;
  }
  char *raw_base = clearPointerID(reservation);

  if (unmapMemory(reservation, range_size) != 0) {
    return 3;
  }

  char *first = mapMemory(raw_base, page_size, MAP_FIXED);
  if (first == NULL) {
    return 4;
  }
  char *second = mapMemory(raw_base + page_size, page_size, MAP_FIXED);
  if (second == NULL) {
    return 5;
  }
  const uint16_t first_id = get_id(first);
  const uint16_t second_id = get_id(second);
  checkLiveBounds(first_id, first, page_size);
  checkLiveBounds(second_id, second, page_size);
  assert(first_id != second_id);

  if (unmapMemory(first, range_size) != 0) {
    return 6;
  }
  checkRetiredBounds(first_id);
  checkRetiredBounds(second_id);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }

  if (strncmp(argv[1], "libc-", 5)) {
    return 2;
  }
  const char *shape = argv[1] + 5;

  if (!strcmp(shape, "repeated")) {
    return testRepeatedUnmap();
  }
  if (!strcmp(shape, "cross")) {
    return testCrossMappingUnmap();
  }
  return testSimpleUnsupportedRange(shape);
}
