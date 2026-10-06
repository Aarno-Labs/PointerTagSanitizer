//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Deferred mremap forms need metadata for both the source and destination.
// This test states the desired public-API behavior rather than expecting the
// temporary fail-closed diagnostic.
//
// XFAIL: *
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -o %t %s
// RUN: %run %t

#define _GNU_SOURCE

#include "../Inputs/ptsan_test_interface.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

static void *clearPointerID(const void *pointer) {
  const uintptr_t address_mask = UINT64_C(0x0000ffffffffffff);
  return (void *)((uintptr_t)pointer & address_mask);
}

int main(void) {
  long page_size_result = sysconf(_SC_PAGESIZE);
  if (page_size_result <= 0) {
    return 1;
  }
  const size_t page_size = (size_t)page_size_result;

  char *source = (char *)mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  char *destination = (char *)mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (source == MAP_FAILED || destination == MAP_FAILED) {
    return 2;
  }
  const uint16_t source_id = get_id(source);
  const uint16_t destination_id = get_id(destination);

  char *result = (char *)mremap(source, page_size, page_size,
                                MREMAP_MAYMOVE | MREMAP_FIXED,
                                clearPointerID(destination));
  if (result == MAP_FAILED) {
    return 3;
  }

  assert(clearPointerID(result) == clearPointerID(destination));
  assert(get_id(result) != source_id);
  assert(get_id(result) != destination_id);
  assert(ptsan_get_base(source_id) == 0);
  assert(ptsan_get_base(destination_id) == 0);
  assert(ptsan_get_base(get_id(result)) == (uint64_t)(uintptr_t)result);
  assert(ptsan_get_size(get_id(result)) == page_size);
  return munmap(result, page_size) != 0;
}
