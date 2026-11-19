//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// ID exhaustion and oversized objects must fail with diagnostics, rather than
// install corrupt allocation metadata. A raw cache hook simulates exhaustion
// without consuming billions of IDs or passing an invalid output buffer.

// RUN: split-file %s %t
// RUN: %clang_raw -O2 -c %t/cache.c -o %t/cache.o
// RUN: %clang_ptsan_id32 -O2 %t/main.c %t/cache.o -Wl,--wrap=_ZN7__ptsan7IDCache4takeEv -o %t/test
// RUN: not %run %t/test ids 2>&1 | FileCheck %s --check-prefix=IDS
// RUN: not %run %t/test size 2>&1 | FileCheck %s --check-prefix=SIZE

// IDS: PTSan ID32 exhausted its ID space
// SIZE: PTSan ID32 cannot represent objects of 2 GiB or larger

//--- main.c
#include <stdint.h>
#include <string.h>

extern void exhaust_stack_ids(void);
extern uint32_t ptsan_get_next_nonstack_id(void *, uint64_t, int, uint64_t);

int main(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "ids") == 0) {
    exhaust_stack_ids();
  }

  char object;
  ptsan_get_next_nonstack_id(&object, 1ULL << 31, 0, 1);
  return 0;
}

//--- cache.c
#include <stdint.h>

extern void ptsan_get_stack_ids(uint32_t *, uint64_t);
static int exhausted;

// Wrap __ptsan::IDCache::take(), preserving normal allocation until the test
// requests exhaustion. The explicit pointer is the C++ method's this argument.
extern uint32_t __real__ZN7__ptsan7IDCache4takeEv(void *);
uint32_t __wrap__ZN7__ptsan7IDCache4takeEv(void *cache) {
  if (exhausted) {
    return 0;
  }
  return __real__ZN7__ptsan7IDCache4takeEv(cache);
}

void exhaust_stack_ids(void) {
  uint32_t id;
  exhausted = 1;
  ptsan_get_stack_ids(&id, 1);
}
