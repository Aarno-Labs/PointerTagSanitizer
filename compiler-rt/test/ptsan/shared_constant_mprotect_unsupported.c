//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan -o %t %s
// RUN: not %run %t 2>&1 | FileCheck %s
// RUN: not %run %t pkey 2>&1 | FileCheck %s
// CHECK: PTSan rejected successfully making shared constant memory writable

#define _GNU_SOURCE

#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifdef PTSAN_TEST_32_BIT_IDS
#include "Inputs/ptsan_test_interface.h"
#endif

static const char ConstantData[] = "shared constant data";

int main(int argc, char **argv) {
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) {
    return 1;
  }

#ifdef PTSAN_TEST_32_BIT_IDS
  uintptr_t address = (uintptr_t)ptsan_get_address(ConstantData);
#else
  uintptr_t address_mask = (UINT64_C(1) << 48) - 1;
  uintptr_t address = (uintptr_t)ConstantData & address_mask;
#endif
  void *page = (void *)(address & ~((uintptr_t)page_size - 1));
  const int use_pkey = argc > 1 && argv[1][0] == 'p';

  errno = 0;
  int failed;
  if (use_pkey) {
    failed = (int)syscall(SYS_pkey_mprotect, (char *)page + 1,
                          (size_t)page_size, PROT_READ | PROT_WRITE, -1);
  } else {
    failed = mprotect((char *)page + 1, (size_t)page_size,
                      PROT_READ | PROT_WRITE);
  }
  if (failed != -1 || errno != EINVAL) {
    return 2;
  }

  if (use_pkey) {
    return syscall(SYS_pkey_mprotect, page, (size_t)page_size,
                   PROT_READ | PROT_WRITE, -1) != 0;
  }
  return mprotect(page, (size_t)page_size, PROT_READ | PROT_WRITE) != 0;
}
