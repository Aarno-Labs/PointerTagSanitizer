// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise current scalar and bounded-buffer prctl operations. Operations that
// are architecture- or kernel-version-specific may return EINVAL; reaching the
// kernel without PTSan rejecting or leaking a tagged pointer is the contract.
//
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror %s -o %t
// RUN: %run %t good
// RUN: not %run %t bad-int 2>&1 | FileCheck %s
// RUN: not %run %t bad-auxv 2>&1 | FileCheck %s
// RUN: %ptsan_musl_id32_strict -Wall -Wextra -Werror %s -o %t.id32
// RUN: %run %t.id32 good
// RUN: not %run %t.id32 bad-cookie 2>&1 | FileCheck %s
// RUN: not %run %t.id32 bad-name 2>&1 | FileCheck %s
// CHECK: Found bad

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>

#ifndef PR_SCHED_CORE
#  define PR_SCHED_CORE 62
#  define PR_SCHED_CORE_GET 0
#endif
#ifndef PR_SME_SET_VL
#  define PR_SME_SET_VL 63
#  define PR_SME_GET_VL 64
#endif
#ifndef PR_SET_MDWE
#  define PR_SET_MDWE 65
#  define PR_GET_MDWE 66
#endif
#ifndef PR_SET_VMA
#  define PR_SET_VMA 0x53564d41
#  define PR_SET_VMA_ANON_NAME 0
#endif
#ifndef PR_GET_AUXV
#  define PR_GET_AUXV 0x41555856
#endif
#ifndef PR_SET_MEMORY_MERGE
#  define PR_SET_MEMORY_MERGE 67
#  define PR_GET_MEMORY_MERGE 68
#endif
#ifndef PR_RISCV_V_SET_CONTROL
#  define PR_RISCV_V_SET_CONTROL 69
#  define PR_RISCV_V_GET_CONTROL 70
#endif

static int test_good(void) {
  int value = 0;
  (void)prctl(PR_GET_UNALIGN, (unsigned long)&value, 0UL, 0UL, 0UL);
  (void)prctl(PR_GET_FPEMU, (unsigned long)&value, 0UL, 0UL, 0UL);
  (void)prctl(PR_GET_FPEXC, (unsigned long)&value, 0UL, 0UL, 0UL);
  (void)prctl(PR_GET_ENDIAN, (unsigned long)&value, 0UL, 0UL, 0UL);

  unsigned char auxiliary[4096];
  (void)prctl(PR_GET_AUXV, (unsigned long)auxiliary, sizeof(auxiliary), 0UL,
              0UL);

  uint64_t cookie = 0;
  (void)prctl(PR_SCHED_CORE, PR_SCHED_CORE_GET, 0UL, 0UL,
              (unsigned long)&cookie);

  size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
  void *mapping = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapping == MAP_FAILED) {
    return 1;
  }
  char name[] = "ptsan-prctl";
  (void)prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, (unsigned long)mapping,
              page_size, (unsigned long)name);
  if (munmap(mapping, page_size)) {
    return 2;
  }

  (void)prctl(PR_SME_SET_VL, 0UL, 0UL, 0UL, 0UL);
  (void)prctl(PR_SME_GET_VL, 0UL, 0UL, 0UL, 0UL);
  (void)prctl(PR_SET_MDWE, 0UL, 0UL, 0UL, 0UL);
  (void)prctl(PR_GET_MDWE, 0UL, 0UL, 0UL, 0UL);
  (void)prctl(PR_SET_MEMORY_MERGE, 0UL, 0UL, 0UL, 0UL);
  (void)prctl(PR_GET_MEMORY_MERGE, 0UL, 0UL, 0UL, 0UL);
  (void)prctl(PR_RISCV_V_SET_CONTROL, 0UL, 0UL, 0UL, 0UL);
  (void)prctl(PR_RISCV_V_GET_CONTROL, 0UL, 0UL, 0UL, 0UL);
  return 0;
}

static int bad_range(const char *mode) {
  if (!strcmp(mode, "bad-int")) {
    void *short_value = malloc(sizeof(int) - 1);
    if (!short_value) {
      return 1;
    }
    return prctl(PR_GET_UNALIGN, (unsigned long)short_value, 0UL, 0UL, 0UL);
  }
  if (!strcmp(mode, "bad-auxv")) {
    void *short_value = malloc(sizeof(uint64_t) - 1);
    if (!short_value) {
      return 2;
    }
    return prctl(PR_GET_AUXV, (unsigned long)short_value, sizeof(uint64_t), 0UL,
                 0UL);
  }
  if (!strcmp(mode, "bad-cookie")) {
    void *short_value = malloc(sizeof(uint64_t) - 1);
    if (!short_value) {
      return 3;
    }
    return prctl(PR_SCHED_CORE, PR_SCHED_CORE_GET, 0UL, 0UL,
                 (unsigned long)short_value);
  }
  if (!strcmp(mode, "bad-name")) {
    char *name = malloc(1);
    if (!name) {
      return 4;
    }
    name[0] = 'x';
    return prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, 0UL, 0UL,
                 (unsigned long)name);
  }
  return 5;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }
  if (!strcmp(argv[1], "good")) {
    return test_good();
  }
  return bad_range(argv[1]);
}
