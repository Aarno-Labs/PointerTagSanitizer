//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan -O2 -fno-builtin -o %t %s
// RUN: %t | FileCheck %s --check-prefix=ORDINARY
// RUN: %clang_ptsan -O2 -fno-builtin -DSTACK_IDS \
// RUN:   -mllvm -ptsan-no-stack-id-fast-path -o %t.stack %s
// RUN: %t.stack | FileCheck %s --check-prefix=ORDINARY
// RUN: %t.stack x | FileCheck %s --check-prefix=ORDINARY
// RUN: %t.stack x x | FileCheck %s --check-prefix=ORDINARY
// RUN: %t.stack x x x | FileCheck %s --check-prefix=ORDINARY
// RUN: %t.stack x x x x | FileCheck %s --check-prefix=ORDINARY

// ORDINARY: default ID produced

// Ordinary mode permits the default-ID fallback after identifier exhaustion.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "Inputs/ptsan_test_interface.h"

#ifdef STACK_IDS
// Reserve IDs without deep recursion, exercising each stack allocation
// helper. The generic helper takes a raw output buffer, so keep this small
// caller uninstrumented and uninlined. argc selects the helper without
// depending on argv strings being tagged in the glibc-wrapper configuration.
extern uint16_t ptsan_get_stack_ids_1(void *, uint64_t);
extern uint32_t ptsan_get_stack_ids_2(void *, void *, uint64_t);
extern uint64_t ptsan_get_stack_ids_3(void *, void *, void *, uint64_t, uint32_t);
extern uint64_t ptsan_get_stack_ids_4(void *, void *, void *, void *, uint64_t,
                                    uint64_t);
extern void ptsan_get_stack_ids_n(uint16_t *, uint64_t);

__attribute__((noinline, disable_sanitizer_instrumentation))
static uint16_t allocate_id(unsigned batch) {
  switch (batch) {
  case 1:
    return ptsan_get_stack_ids_1(0, 0);
  case 2:
    return ptsan_get_stack_ids_2(0, 0, 0);
  case 3:
    return ptsan_get_stack_ids_3(0, 0, 0, 0, 0);
  case 4:
    return ptsan_get_stack_ids_4(0, 0, 0, 0, 0, 0);
  default: {
    uint16_t ids[5];
    ptsan_get_stack_ids_n(ids, 5);
    return ids[0];
  }
  }
}
#else
static uint16_t allocate_id(unsigned batch) {
  void *p = malloc(1);
  assert(p);
  return get_id(p);
}
#endif

int main(int argc, char **argv) {
  for (unsigned i = 0; i < 70000; ++i) {
    if (allocate_id(argc - 1) == ptsan_default_id()) {
      puts("default ID produced");
      return 0;
    }
  }
  abort();
}
