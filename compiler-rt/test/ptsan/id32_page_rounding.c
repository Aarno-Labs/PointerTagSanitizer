//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-random-seed=1 %s -o %t
// RUN: %run %t
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-stack-temporal-safety=0 %s -o %t.weak
// RUN: %run %t.weak

// Neither buffer requests page alignment. Rounding an encoded interior pointer
// must still produce a page-aligned address after the syscall interceptor
// decodes it; merely making the encoded allocation base aligned is not enough.
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

__attribute__((noinline)) void check_page(char *buffer, size_t page_size) {
  uintptr_t aligned =
      ((uintptr_t)(buffer + 17) + page_size - 1) & ~(page_size - 1);
  volatile char *page = (volatile char *)aligned;
  page[0] = 7;
  page[page_size - 1] = 11;
  assert(syscall(SYS_madvise, (void *)aligned, page_size, MADV_NORMAL) == 0);
  assert(page[0] == 7 && page[page_size - 1] == 11);
}

int main(void) {
  size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
  // Large enough for three pages on either target, without page alignment.
  char stack[3 * 65536];
  check_page(stack, page_size);
  for (unsigned i = 0; i < 16; ++i) {
    char *heap = malloc(3 * page_size);
    assert(heap != NULL);
    check_page(heap, page_size);
    free(heap);
  }
  return 0;
}
