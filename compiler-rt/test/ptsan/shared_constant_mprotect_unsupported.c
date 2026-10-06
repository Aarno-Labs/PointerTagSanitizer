//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// XFAIL: *
// RUN: %clang_ptsan -o %t %s
// RUN: %run %t

#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

static const char ConstantData[] = "shared constant data";

int main(void) {
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) {
    return 1;
  }

  uintptr_t address_mask = (UINT64_C(1) << 48) - 1;
  uintptr_t address = (uintptr_t)ConstantData & address_mask;
  void *page = (void *)(address & ~((uintptr_t)page_size - 1));
  return mprotect(page, (size_t)page_size, PROT_READ | PROT_WRITE) != 0;
}
