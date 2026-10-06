//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise ownership and metadata transitions for the public free(),
// munmap(), and syscall() interfaces.
// This test intentionally uses PTSan's read-only test interface to assert the
// exact internal bounds state after each transition.
//
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -o %t %s
// RUN: %run %t valid-free
// RUN: not %run %t invalid-free 2> %t.invalid-free
// RUN: FileCheck %s --check-prefix=INVALID-FREE < %t.invalid-free
// RUN: %run %t libc-failed
// RUN: %run %t syscall-failed
// RUN: %run %t syscall-repeated
//
// RUN: not %run %t libc-whole 2> %t.libc-whole
// RUN: FileCheck %s --check-prefix=STALE < %t.libc-whole
//
// RUN: not %run %t syscall-whole 2> %t.syscall-whole
// RUN: FileCheck %s --check-prefix=STALE < %t.syscall-whole
//
// INVALID-FREE: INVALID-FREE-READY
// INVALID-FREE: {{PT[Ss]an found .*free}}
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

static size_t pageSize(void) {
  long result = sysconf(_SC_PAGESIZE);
  if (result <= 0) {
    return 0;
  }
  return (size_t)result;
}

static char *mapMemory(void *address, size_t size, int additional_flags) {
  void *result = mmap(address, size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | additional_flags, -1, 0);
  if (result == MAP_FAILED) {
    return NULL;
  }
  return (char *)result;
}

static int unmapMemory(bool use_syscall, void *address, size_t size) {
  if (use_syscall) {
    return (int)syscall(SYS_munmap, address, size);
  }
  return munmap(address, size);
}

__attribute__((noinline)) static char readStaleByte(const volatile char *ptr) {
  return *ptr;
}

static int testValidFree(void) {
  // Musl's mallocng serves allocations of this size from a dedicated mapping,
  // so free() reaches its internal munmap path while owning the malloc ID.
  const size_t allocation_size = 1024 * 1024;
  volatile unsigned char *allocation =
      (volatile unsigned char *)malloc(allocation_size);
  if (allocation == NULL) {
    return 1;
  }

  const uint16_t allocation_id = get_id(allocation);
  checkLiveBounds(allocation_id, (const void *)allocation, allocation_size);

  allocation[0] = 1;
  allocation[allocation_size - 1] = 2;
  free((void *)allocation);
  checkRetiredBounds(allocation_id);
  return 0;
}

__attribute__((noinline)) static void freeInteriorPointer(void *pointer) {
  free(pointer);
}

static int testInvalidFree(void) {
  char *allocation = (char *)malloc(128);
  if (allocation == NULL) {
    return 1;
  }

  checkLiveBounds(get_id(allocation), allocation, 128);

  fprintf(stderr, "INVALID-FREE-READY\n");
  freeInteriorPointer(allocation + 1);
  return 2;
}

static int testFailedUnmaps(bool use_syscall) {
  const size_t page_size = pageSize();
  if (page_size == 0) {
    return 1;
  }

  const size_t mapping_size = 2 * page_size;
  volatile char *mapping = (volatile char *)mapMemory(NULL, mapping_size, 0);
  if (mapping == NULL) {
    return 2;
  }

  const uint16_t mapping_id = get_id(mapping);
  checkLiveBounds(mapping_id, (const void *)mapping, mapping_size);

  mapping[0] = 1;
  mapping[page_size] = 2;

  // This reaches the kernel with the exact tracked base. A failed syscall must
  // not retire its metadata.
  errno = 0;
  if (unmapMemory(use_syscall, (void *)mapping, 0) != -1 || errno != EINVAL) {
    return 3;
  }
  checkLiveBounds(mapping_id, (const void *)mapping, mapping_size);
  mapping[0] = 3;

  if (unmapMemory(use_syscall, (void *)mapping, mapping_size) != 0) {
    return 4;
  }
  checkRetiredBounds(mapping_id);
  return 0;
}

static int testSuccessfulUnmap(bool use_syscall) {
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

  volatile char *stale_pointer = mapping;

  if (unmapMemory(use_syscall, mapping, mapping_size) != 0) {
    return 3;
  }

  checkRetiredBounds(mapping_id);

  char *replacement = mapMemory(mapping, mapping_size, MAP_FIXED);
  if (replacement == NULL) {
    return 4;
  }
  const uint16_t replacement_id = get_id(replacement);
  assert(replacement_id != mapping_id);
  checkLiveBounds(replacement_id, replacement, mapping_size);
  checkRetiredBounds(mapping_id);
  replacement[0] = 42;

  // Remapping first distinguishes stale PTSan metadata from an ordinary
  // hardware fault on an unmapped page.
  fprintf(stderr, "STALE-READY\n");
  volatile char secret = readStaleByte(stale_pointer);
  (void)secret;
  return 5;
}

static int testRepeatedDirectUnmap(void) {
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

  if (unmapMemory(true, mapping, page_size) != 0) {
    return 3;
  }
  checkRetiredBounds(mapping_id);

  // Linux accepts a range that is already unmapped. The raw handler must
  // preserve the retired metadata while allowing the syscall to proceed.
  if (unmapMemory(true, mapping, page_size) != 0) {
    return 4;
  }
  checkRetiredBounds(mapping_id);
  return 0;
}

static int testNamedUnmap(const char *mode) {
  if (!strcmp(mode, "libc-whole")) {
    return testSuccessfulUnmap(false);
  }
  if (!strcmp(mode, "syscall-whole")) {
    return testSuccessfulUnmap(true);
  }
  return 1;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }
  if (!strcmp(argv[1], "valid-free")) {
    return testValidFree();
  }
  if (!strcmp(argv[1], "invalid-free")) {
    return testInvalidFree();
  }
  if (!strcmp(argv[1], "libc-failed")) {
    return testFailedUnmaps(false);
  }
  if (!strcmp(argv[1], "syscall-failed")) {
    return testFailedUnmaps(true);
  }
  if (!strcmp(argv[1], "syscall-repeated")) {
    return testRepeatedDirectUnmap();
  }
  return testNamedUnmap(argv[1]);
}
