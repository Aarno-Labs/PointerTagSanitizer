//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise allocator APIs which cannot rely entirely on the primary malloc
// wrappers: posix_memalign() writes through a caller-owned output pointer, and
// malloc_usable_size() consumes an allocation pointer directly.
//
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -fno-builtin -o %t %s
// RUN: %run %t good
// RUN: not %run %t bad 2> %t.bad
// RUN: FileCheck %s < %t.bad
//
// CHECK: BAD-READY
// CHECK: Found bad

#include <assert.h>
#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *allocate(void) {
  const size_t alignment = 64;
  const size_t requested_size = 123;

  void *failed_result = (void *)(uintptr_t)1;
  assert(posix_memalign(&failed_result, 3, requested_size) == EINVAL);
  assert(failed_result == (void *)(uintptr_t)1);

  void **result = (void **)malloc(sizeof(void *));
  assert(result != NULL);
  assert(posix_memalign(result, alignment, requested_size) == 0);
  assert(*result != NULL);
  assert((uintptr_t)*result % alignment == 0);

  const size_t usable_size = malloc_usable_size(*result);
  assert(usable_size >= requested_size);

  unsigned char *allocation = (unsigned char *)*result;
  free(result);
  return allocation;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }

  unsigned char *allocation = allocate();
  if (!strcmp(argv[1], "good")) {
    allocation[0] = 1;
    allocation[122] = 2;
    free(allocation);
    return 0;
  }
  if (!strcmp(argv[1], "bad")) {
    fprintf(stderr, "BAD-READY\n");
    allocation[123] = 1;
    return 0;
  }

  free(allocation);
  return 2;
}
