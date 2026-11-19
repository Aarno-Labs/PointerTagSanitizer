//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// In-place growth must preserve aliases, including when the enlarged mapping
// would overflow the positive headroom of an unrestricted randomized origin.
// Only a single page is touched; the large growth reserves virtual space.
// RUN: %ptsan_musl_id32_strict -mllvm -ptsan-random-seed=1 %s -o %t
// RUN: %run %t

#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

__attribute__((noinline)) static unsigned char
read_byte(volatile unsigned char *p) {
  return *p;
}

int main(void) {
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  const size_t grown_size = (size_t)1 << 30;

  for (unsigned attempt = 0; attempt < 64; ++attempt) {
    // Find an empty growth range, then map only its first page.
    unsigned char *reservation = mmap(NULL, grown_size, PROT_NONE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(reservation != MAP_FAILED);
    assert(munmap(reservation, grown_size) == 0);
    unsigned char *mapping = mmap(reservation, page, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                                 -1, 0);
    assert(mapping != MAP_FAILED);

    unsigned char *saved = mapping + 123;
    *saved = 0x5a;
    assert(read_byte(saved) == 0x5a);
    // Without MREMAP_MAYMOVE, success guarantees the mapping did not move.
    unsigned char *grown = mremap(mapping, page, grown_size, 0);
    assert(grown != MAP_FAILED);
    assert(read_byte(grown + 123) == 0x5a);
    assert(read_byte(saved) == 0x5a);
    grown[123] = 0xa5;
    assert(read_byte(saved) == 0xa5);
    assert(munmap(grown, grown_size) == 0);
  }
  puts("all saved aliases survived");
  return 0;
}
