//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise SysV shared-memory attachment metadata through public musl APIs.
//
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -o %t %s
// RUN: %run %t libc-good
// RUN: %run %t syscall-good
// RUN: %run %t libc-multiple
// RUN: %run %t syscall-multiple
// RUN: %run %t libc-failed-detach
// RUN: %run %t syscall-failed-detach
//
// RUN: not %run %t libc-bad 2> %t.libc-bad
// RUN: FileCheck %s --check-prefix=BOUNDS < %t.libc-bad
// RUN: not %run %t syscall-bad 2> %t.syscall-bad
// RUN: FileCheck %s --check-prefix=BOUNDS < %t.syscall-bad
// RUN: not %run %t libc-stale 2> %t.libc-stale
// RUN: FileCheck %s --check-prefix=STALE < %t.libc-stale
// RUN: not %run %t syscall-stale 2> %t.syscall-stale
// RUN: FileCheck %s --check-prefix=STALE < %t.syscall-stale
//
// BOUNDS: BOUNDS-READY
// BOUNDS: Found bad
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
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
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

static char *attachMemory(bool use_syscall, int shared_memory_id,
                          const void *requested_address, int flags) {
  if (use_syscall) {
    long result =
        syscall(SYS_shmat, shared_memory_id, requested_address, flags);
    if (result == -1) {
      return NULL;
    }
    return (char *)result;
  }

  void *result = shmat(shared_memory_id, requested_address, flags);
  if (result == (void *)-1) {
    return NULL;
  }
  return (char *)result;
}

static int detachMemory(bool use_syscall, const void *address) {
  if (use_syscall) {
    return (int)syscall(SYS_shmdt, address);
  }
  return shmdt(address);
}

static int createSegment(size_t size) {
  return shmget(IPC_PRIVATE, size, IPC_CREAT | 0600);
}

static int removeSegment(int shared_memory_id) {
  return shmctl(shared_memory_id, IPC_RMID, NULL);
}

__attribute__((noinline)) static char readByte(const volatile char *ptr) {
  return *ptr;
}

static int testGood(bool use_syscall) {
  const size_t page_size = pageSize();
  if (page_size == 0) {
    return 1;
  }
  const size_t segment_size = page_size + 1;

  int shared_memory_id = createSegment(segment_size);
  if (shared_memory_id < 0) {
    return 2;
  }
  char *mapping = attachMemory(use_syscall, shared_memory_id, NULL, 0);
  if (mapping == NULL) {
    removeSegment(shared_memory_id);
    return 3;
  }
  if (removeSegment(shared_memory_id) != 0) {
    return 4;
  }

  const uint16_t mapping_id = get_id(mapping);
  checkLiveBounds(mapping_id, mapping, segment_size);
  mapping[0] = 1;
  mapping[segment_size - 1] = 2;

  if (detachMemory(use_syscall, mapping) != 0) {
    return 5;
  }
  checkRetiredBounds(mapping_id);
  return 0;
}

static int testBadBounds(bool use_syscall) {
  const size_t page_size = pageSize();
  if (page_size == 0) {
    return 1;
  }
  const size_t segment_size = page_size + 1;

  int shared_memory_id = createSegment(segment_size);
  if (shared_memory_id < 0) {
    return 2;
  }
  char *mapping = attachMemory(use_syscall, shared_memory_id, NULL, 0);
  if (mapping == NULL) {
    removeSegment(shared_memory_id);
    return 3;
  }
  if (removeSegment(shared_memory_id) != 0) {
    return 4;
  }

  checkLiveBounds(get_id(mapping), mapping, segment_size);
  mapping[segment_size - 1] = 1;
  fprintf(stderr, "BOUNDS-READY\n");
  volatile char secret = readByte(mapping + segment_size);
  (void)secret;
  return 5;
}

static int testMultipleAttachments(bool use_syscall) {
  const size_t page_size = pageSize();
  if (page_size == 0) {
    return 1;
  }

  int shared_memory_id = createSegment(page_size);
  if (shared_memory_id < 0) {
    return 2;
  }
  char *first = attachMemory(use_syscall, shared_memory_id, NULL, 0);
  char *second = attachMemory(use_syscall, shared_memory_id, NULL, 0);
  if (first == NULL || second == NULL) {
    removeSegment(shared_memory_id);
    return 3;
  }
  if (removeSegment(shared_memory_id) != 0) {
    return 4;
  }

  const uint16_t first_id = get_id(first);
  const uint16_t second_id = get_id(second);
  assert(first_id != second_id);
  checkLiveBounds(first_id, first, page_size);
  checkLiveBounds(second_id, second, page_size);

  first[0] = 42;
  assert(second[0] == 42);
  if (detachMemory(use_syscall, first) != 0) {
    return 5;
  }
  checkRetiredBounds(first_id);
  checkLiveBounds(second_id, second, page_size);
  assert(second[0] == 42);

  if (detachMemory(use_syscall, second) != 0) {
    return 6;
  }
  checkRetiredBounds(second_id);
  return 0;
}

static int testFailedDetach(bool use_syscall) {
  const size_t page_size = pageSize();
  if (page_size == 0) {
    return 1;
  }

  char *mapping = (char *)mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapping == MAP_FAILED) {
    return 2;
  }
  const uint16_t mapping_id = get_id(mapping);
  checkLiveBounds(mapping_id, mapping, page_size);

  errno = 0;
  if (detachMemory(use_syscall, mapping) != -1 || errno != EINVAL) {
    return 3;
  }
  checkLiveBounds(mapping_id, mapping, page_size);
  mapping[0] = 1;

  if (munmap(mapping, page_size) != 0) {
    return 4;
  }
  checkRetiredBounds(mapping_id);
  return 0;
}

static int testStalePointer(bool use_syscall) {
  const size_t page_size = pageSize();
  if (page_size == 0) {
    return 1;
  }

  int shared_memory_id = createSegment(page_size);
  if (shared_memory_id < 0) {
    return 2;
  }
  char *mapping = attachMemory(use_syscall, shared_memory_id, NULL, 0);
  if (mapping == NULL) {
    removeSegment(shared_memory_id);
    return 3;
  }
  if (removeSegment(shared_memory_id) != 0) {
    return 4;
  }

  const uint16_t old_id = get_id(mapping);
  checkLiveBounds(old_id, mapping, page_size);
  volatile char *stale_pointer = mapping;
  void *raw_address = clearPointerID(mapping);

  if (detachMemory(use_syscall, mapping) != 0) {
    return 5;
  }
  checkRetiredBounds(old_id);

  char *replacement =
      (char *)mmap(raw_address, page_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  if (replacement == MAP_FAILED) {
    return 6;
  }
  const uint16_t replacement_id = get_id(replacement);
  assert(replacement_id != old_id);
  checkLiveBounds(replacement_id, replacement, page_size);
  checkRetiredBounds(old_id);
  replacement[0] = 42;

  fprintf(stderr, "STALE-READY\n");
  volatile char secret = readByte(stale_pointer);
  (void)secret;
  return 7;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }
  bool use_syscall = false;
  const char *operation;
  if (!strncmp(argv[1], "syscall-", 8)) {
    use_syscall = true;
    operation = argv[1] + 8;
  } else if (!strncmp(argv[1], "libc-", 5)) {
    operation = argv[1] + 5;
  } else {
    return 2;
  }
  if (!strcmp(operation, "good")) {
    return testGood(use_syscall);
  }
  if (!strcmp(operation, "bad")) {
    return testBadBounds(use_syscall);
  }
  if (!strcmp(operation, "multiple")) {
    return testMultipleAttachments(use_syscall);
  }
  if (!strcmp(operation, "failed-detach")) {
    return testFailedDetach(use_syscall);
  }
  if (!strcmp(operation, "stale")) {
    return testStalePointer(use_syscall);
  }
  return 2;
}
