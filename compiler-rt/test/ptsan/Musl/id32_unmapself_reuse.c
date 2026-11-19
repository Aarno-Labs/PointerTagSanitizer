//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Thread teardown must decode its mapping before retiring the ID. Replace its
// decode metadata at retirement and check the final unmap boundary's address.

// REQUIRES: ptsan-instrumented-musl
// RUN: %ptsan_musl_id32_strict -fno-sanitize=ptsan -DTEST_UNMAPSELF -c %S/../Inputs/id32_deallocation_reuse.c -o %t.probe.o
// RUN: %ptsan_musl_id32_strict %s %t.probe.o -Wl,--wrap=ptsan_retire_mmap_if_exact_direct -o %t
// RUN: %run %t

#include <assert.h>
#include <stddef.h>
#include <sys/mman.h>

void prepare_reuse(void *);
__attribute__((noreturn)) void __wrap___unmapself(void *, size_t);

int main(void) {
  void *ptr = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(ptr != MAP_FAILED);
  *(volatile char *)ptr = 42;
  prepare_reuse(ptr);
  __wrap___unmapself(ptr, 4096);
}
