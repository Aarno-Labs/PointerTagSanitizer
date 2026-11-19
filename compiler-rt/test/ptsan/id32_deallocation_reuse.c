//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Free and munmap must use an address decoded before ID retirement. The raw
// probe replaces decode metadata during retirement, exposing any later decode
// that would incorrectly use the replacement object's metadata.

// RUN: %clang_raw -O2 -DPTSAN_TEST_32_BIT_IDS -c %S/Inputs/id32_deallocation_reuse.c -o %t.probe.o
// RUN: %clang_ptsan_id32 -O2 %s %t.probe.o -Wl,--wrap=ptsan_mark_ptr_freed -o %t
// RUN: %run %t
// RUN: %run %t munmap

#include <assert.h>
#include <stdlib.h>
#include <sys/mman.h>

void prepare_reuse(void *);
void check_deallocation(int);

int main(int argc, char **argv) {
  int unmap = argc > 1;
  void *ptr = unmap ? mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)
                   : malloc(16);
  assert(ptr != NULL && ptr != MAP_FAILED);
  *(volatile char *)ptr = 42;
  prepare_reuse(ptr);
  if (unmap) {
    assert(munmap(ptr, 4096) == 0);
  } else {
    free(ptr);
  }
  check_deallocation(unmap);
  return 0;
}
