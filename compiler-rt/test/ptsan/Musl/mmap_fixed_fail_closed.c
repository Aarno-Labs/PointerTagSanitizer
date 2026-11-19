//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// MAP_FIXED can discard storage underlying multiple PTSan allocations, and
// PTSan cannot enumerate every allocation ID that must be retired. This test
// uses the simplest case--replacement of one directly tracked mmap--and checks
// that PTSan rejects the successful operation in every mode. Ordinary syscall
// failures and MAP_FIXED_NOREPLACE remain supported.
//
// RUN: %ptsan_musl_nonstrict -fno-sanitize=ptsan -Wall -Wextra -Werror -c %S/Inputs/mmap_fixed_in_libc.c -o %t.helper.o
// RUN: %ptsan_musl_nonstrict -Wall -Wextra -Werror -o %t.nonstrict %s %t.helper.o
// RUN: not %run %t.nonstrict 2>&1 | FileCheck %s
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -o %t %s %t.helper.o
// RUN: not %run %t 2>&1 | FileCheck %s
// RUN: %ptsan_musl_id32_nonstrict -Wall -Wextra -Werror -o %t.id32.nonstrict %s %t.helper.o
// RUN: not %run %t.id32.nonstrict 2>&1 | FileCheck %s
// RUN: %ptsan_musl_id32_strict -Wall -Wextra -Werror -o %t.id32 %s %t.helper.o
// RUN: not %run %t.id32 2>&1 | FileCheck %s
//
// CHECK: PTSan rejected successful unsupported MAP_FIXED

#define _GNU_SOURCE

#include <errno.h>
#include <stddef.h>
#include <sys/mman.h>
#include <unistd.h>

void ptsan_test_enter_libc(void);
void ptsan_test_leave_libc(void);

int main(void) {
  const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
  void *mapping = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapping == MAP_FAILED)
    return 1;

  errno = 0;
  void *failed = mmap((char *)mapping + 1, page_size,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  if (failed != MAP_FAILED || errno != EINVAL)
    return 2;

  ptsan_test_enter_libc();
  void *replacement = mmap(mapping, page_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  ptsan_test_leave_libc();
  return replacement == MAP_FAILED;
}
