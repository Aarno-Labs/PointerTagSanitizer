//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Deferred direct-syscall partial, tagless, and cross-object munmap coverage.
// Supporting these requires range-aware metadata updates in the raw syscall
// handler; public munmap intentionally remains fail-closed for these shapes.
// This test intentionally uses PTSan's read-only test interface to specify the
// exact internal bounds state that the deferred implementation must produce.
//
// XFAIL: *
// RUN: rm -f %t.deferred-failed
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -o %t %s
// RUN: ( not %run %t syscall-prefix 2> %t.syscall-prefix && FileCheck %s < %t.syscall-prefix ) || touch %t.deferred-failed
// RUN: ( not %run %t syscall-suffix 2> %t.syscall-suffix && FileCheck %s < %t.syscall-suffix ) || touch %t.deferred-failed
// RUN: ( not %run %t syscall-middle 2> %t.syscall-middle && FileCheck %s < %t.syscall-middle ) || touch %t.deferred-failed
// RUN: ( not %run %t syscall-tagless-whole 2> %t.syscall-tagless-whole && FileCheck %s < %t.syscall-tagless-whole ) || touch %t.deferred-failed
// RUN: ( not %run %t syscall-tagless-prefix 2> %t.syscall-tagless-prefix && FileCheck %s < %t.syscall-tagless-prefix ) || touch %t.deferred-failed
// RUN: ( not %run %t syscall-tagless-suffix 2> %t.syscall-tagless-suffix && FileCheck %s < %t.syscall-tagless-suffix ) || touch %t.deferred-failed
// RUN: ( not %run %t syscall-tagless-middle 2> %t.syscall-tagless-middle && FileCheck %s < %t.syscall-tagless-middle ) || touch %t.deferred-failed
// RUN: ( not %run %t syscall-cross-adjacent 2> %t.syscall-cross-adjacent && FileCheck %s < %t.syscall-cross-adjacent ) || touch %t.deferred-failed
// RUN: ( not %run %t syscall-cross-partial 2> %t.syscall-cross-partial && FileCheck %s < %t.syscall-cross-partial ) || touch %t.deferred-failed
// RUN: ( not %run %t syscall-cross-hole 2> %t.syscall-cross-hole && FileCheck %s < %t.syscall-cross-hole ) || touch %t.deferred-failed
// RUN: test ! -e %t.deferred-failed
//
// CHECK: STALE-READY
// CHECK: Found bad

#define _GNU_SOURCE

#include "../Inputs/ptsan_test_interface.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

enum UnmapShape {
  WHOLE_UNMAP,
  PREFIX_UNMAP,
  SUFFIX_UNMAP,
  MIDDLE_UNMAP,
};

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

static void checkBoundsAfterUnmap(uint16_t id, char *mapping, size_t page_size,
                                  enum UnmapShape shape) {
  switch (shape) {
  case WHOLE_UNMAP:
  case MIDDLE_UNMAP:
    checkRetiredBounds(id);
    break;
  case PREFIX_UNMAP:
    checkLiveBounds(id, mapping + page_size, 3 * page_size);
    break;
  case SUFFIX_UNMAP:
    checkLiveBounds(id, mapping, 3 * page_size);
    break;
  }
}

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

static char *mapMemory(void *address, size_t size, int additional_flags) {
  void *result = mmap(address, size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | additional_flags, -1, 0);
  if (result == MAP_FAILED) {
    return NULL;
  }
  return (char *)result;
}

static int unmapMemory(void *address, size_t size) {
  return (int)syscall(SYS_munmap, address, size);
}

__attribute__((noinline)) static char readStaleByte(const volatile char *ptr) {
  return *ptr;
}

static int testShapedUnmap(bool use_tagless_address, enum UnmapShape shape) {
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
  char *raw_mapping = clearPointerID(mapping);
  char *unmap_base = mapping;
  if (use_tagless_address) {
    unmap_base = raw_mapping;
  }

  size_t unmap_offset = 0;
  size_t unmap_size = mapping_size;
  size_t stale_offset = 0;
  switch (shape) {
  case WHOLE_UNMAP:
    break;
  case PREFIX_UNMAP:
    unmap_size = page_size;
    stale_offset = page_size - 1;
    break;
  case SUFFIX_UNMAP:
    unmap_offset = 3 * page_size;
    unmap_size = page_size;
    stale_offset = unmap_offset;
    break;
  case MIDDLE_UNMAP:
    unmap_offset = page_size;
    unmap_size = page_size;
    stale_offset = unmap_offset;
    break;
  }

  char *unmap_address = unmap_base + unmap_offset;
  volatile char *stale_pointer = mapping + stale_offset;
  if (unmapMemory(unmap_address, unmap_size) != 0) {
    return 3;
  }

  checkBoundsAfterUnmap(mapping_id, mapping, page_size, shape);

  if (shape == PREFIX_UNMAP) {
    *(volatile char *)(mapping + page_size) = 1;
  }
  if (shape == SUFFIX_UNMAP) {
    *(volatile char *)mapping = 2;
  }

  char *replacement = mapMemory(unmap_address, page_size, MAP_FIXED);
  if (replacement == NULL) {
    return 4;
  }
  const uint16_t replacement_id = get_id(replacement);
  assert(replacement_id != mapping_id);
  checkLiveBounds(replacement_id, replacement, page_size);
  checkBoundsAfterUnmap(mapping_id, mapping, page_size, shape);
  replacement[stale_offset - unmap_offset] = 42;

  fprintf(stderr, "STALE-READY\n");
  volatile char secret = readStaleByte(stale_pointer);
  (void)secret;
  return 5;
}

static int testCrossMappingUnmap(bool include_hole) {
  const size_t page_size = pageSize();
  if (page_size == 0) {
    return 1;
  }

  const size_t page_count = include_hole ? 3 : 2;
  const size_t range_size = page_count * page_size;
  char *reservation = mapMemory(NULL, range_size, 0);
  if (reservation == NULL) {
    return 2;
  }

  const uint16_t reservation_id = get_id(reservation);
  checkLiveBounds(reservation_id, reservation, range_size);
  char *raw_base = clearPointerID(reservation);
  if (munmap(reservation, range_size) != 0) {
    return 3;
  }
  checkRetiredBounds(reservation_id);

  char *first = mapMemory(raw_base, page_size, MAP_FIXED);
  if (first == NULL) {
    return 4;
  }

  const uint16_t first_id = get_id(first);
  checkRetiredBounds(reservation_id);
  checkLiveBounds(first_id, first, page_size);
  const size_t second_offset = (page_count - 1) * page_size;
  char *second = mapMemory(raw_base + second_offset, page_size, MAP_FIXED);
  if (second == NULL) {
    return 5;
  }

  const uint16_t second_id = get_id(second);
  checkLiveBounds(first_id, first, page_size);
  checkRetiredBounds(reservation_id);
  checkLiveBounds(second_id, second, page_size);
  volatile char *stale_second = second;

  // The starting ID identifies only the first mapping. Correct retirement of
  // the second mapping requires enumerating the entire raw address range.
  if (unmapMemory(first, range_size) != 0) {
    return 6;
  }
  checkRetiredBounds(first_id);
  checkRetiredBounds(second_id);

  char *replacement = mapMemory(raw_base, range_size, MAP_FIXED);
  if (replacement == NULL) {
    return 7;
  }
  const uint16_t replacement_id = get_id(replacement);
  assert(replacement_id != first_id);
  assert(replacement_id != second_id);
  checkLiveBounds(replacement_id, replacement, range_size);
  checkRetiredBounds(first_id);
  checkRetiredBounds(second_id);
  replacement[second_offset] = 42;

  fprintf(stderr, "STALE-READY\n");
  volatile char secret = readStaleByte(stale_second);
  (void)secret;
  return 8;
}

static int testCrossPartialUnmap(void) {
  const size_t page_size = pageSize();
  if (page_size == 0) {
    return 1;
  }

  const size_t mapping_size = 3 * page_size;
  const size_t reservation_size = 2 * mapping_size;
  char *reservation = mapMemory(NULL, reservation_size, 0);
  if (reservation == NULL) {
    return 2;
  }

  const uint16_t reservation_id = get_id(reservation);
  checkLiveBounds(reservation_id, reservation, reservation_size);
  char *raw_base = clearPointerID(reservation);
  if (munmap(reservation, reservation_size) != 0) {
    return 3;
  }
  checkRetiredBounds(reservation_id);

  char *first = mapMemory(raw_base, mapping_size, MAP_FIXED);
  if (first == NULL) {
    return 4;
  }

  const uint16_t first_id = get_id(first);
  checkLiveBounds(first_id, first, mapping_size);
  checkRetiredBounds(reservation_id);

  char *second_raw_base = raw_base + mapping_size;
  char *second = mapMemory(second_raw_base, mapping_size, MAP_FIXED);
  if (second == NULL) {
    return 5;
  }

  const uint16_t second_id = get_id(second);
  assert(first_id != second_id);
  checkLiveBounds(first_id, first, mapping_size);
  checkLiveBounds(second_id, second, mapping_size);
  checkRetiredBounds(reservation_id);

  const size_t unmap_offset = 2 * page_size;
  const size_t unmap_size = 2 * page_size;
  char *unmap_address = first + unmap_offset;
  volatile char *stale_second = second;
  if (unmapMemory(unmap_address, unmap_size) != 0) {
    return 6;
  }
  checkLiveBounds(first_id, first, 2 * page_size);
  checkLiveBounds(second_id, second + page_size, 2 * page_size);

  *(volatile char *)first = 1;
  *(volatile char *)(second + page_size) = 2;

  char *replacement = mapMemory(raw_base + unmap_offset, unmap_size, MAP_FIXED);
  if (replacement == NULL) {
    return 7;
  }

  const uint16_t replacement_id = get_id(replacement);
  assert(replacement_id != first_id);
  assert(replacement_id != second_id);
  checkLiveBounds(replacement_id, replacement, unmap_size);
  checkLiveBounds(first_id, first, 2 * page_size);
  checkLiveBounds(second_id, second + page_size, 2 * page_size);
  *(volatile char *)(replacement + page_size) = 42;

  fprintf(stderr, "STALE-READY\n");
  volatile char secret = readStaleByte(stale_second);
  (void)secret;
  return 8;
}

static int testNamedUnmap(const char *mode) {
  if (!strcmp(mode, "prefix")) {
    return testShapedUnmap(false, PREFIX_UNMAP);
  }
  if (!strcmp(mode, "suffix")) {
    return testShapedUnmap(false, SUFFIX_UNMAP);
  }
  if (!strcmp(mode, "middle")) {
    return testShapedUnmap(false, MIDDLE_UNMAP);
  }
  if (!strcmp(mode, "tagless-whole")) {
    return testShapedUnmap(true, WHOLE_UNMAP);
  }
  if (!strcmp(mode, "tagless-prefix")) {
    return testShapedUnmap(true, PREFIX_UNMAP);
  }
  if (!strcmp(mode, "tagless-suffix")) {
    return testShapedUnmap(true, SUFFIX_UNMAP);
  }
  if (!strcmp(mode, "tagless-middle")) {
    return testShapedUnmap(true, MIDDLE_UNMAP);
  }
  if (!strcmp(mode, "cross-adjacent")) {
    return testCrossMappingUnmap(false);
  }
  if (!strcmp(mode, "cross-partial")) {
    return testCrossPartialUnmap();
  }
  if (!strcmp(mode, "cross-hole")) {
    return testCrossMappingUnmap(true);
  }
  return 1;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }
  if (!strncmp(argv[1], "syscall-", 8)) {
    return testNamedUnmap(argv[1] + 8);
  }
  return 2;
}
