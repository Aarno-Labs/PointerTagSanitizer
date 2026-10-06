//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise mincore mapping addresses and counted output vectors via musl.
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t good
// RUN: not %run %t bad-vector 2> %t.vector
// RUN: FileCheck %s < %t.vector
//
// CHECK: Found bad

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int test_good(void) {
  long page_size_value = sysconf(_SC_PAGESIZE);
  if (page_size_value <= 0) {
    return 1;
  }
  size_t page_size = (size_t)page_size_value;
  size_t mapping_size = 2 * page_size;

  void *mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  unsigned char *vector = malloc(3);
  if (mapping == MAP_FAILED || !vector) {
    free(vector);
    if (mapping != MAP_FAILED) {
      munmap(mapping, mapping_size);
    }
    return 2;
  }

  if (mincore(mapping, page_size + 1, vector) != 0) {
    goto fail;
  }

  if (mincore(mapping, 0, vector + 3) != 0) {
    goto fail;
  }

  // The queried range is a virtual-memory range, not a PTSan object range.
  (void)mincore((char *)mapping + mapping_size, page_size, vector);

  free(vector);
  munmap(mapping, mapping_size);
  return 0;

fail:
  free(vector);
  munmap(mapping, mapping_size);
  return 3;
}

static int test_bad_vector(void) {
  long page_size_value = sysconf(_SC_PAGESIZE);
  if (page_size_value <= 0) {
    return 1;
  }
  size_t page_size = (size_t)page_size_value;
  size_t mapping_size = 2 * page_size;

  void *mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  unsigned char *vector = malloc(1);
  if (mapping == MAP_FAILED || !vector) {
    free(vector);
    if (mapping != MAP_FAILED) {
      munmap(mapping, mapping_size);
    }
    return 2;
  }

  (void)mincore(mapping, page_size + 1, vector);
  free(vector);
  munmap(mapping, mapping_size);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }
  if (!strcmp(argv[1], "good")) {
    return test_good();
  }
  if (!strcmp(argv[1], "bad-vector")) {
    return test_bad_vector();
  }
  return 2;
}
