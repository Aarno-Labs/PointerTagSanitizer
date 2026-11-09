//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// The stack ownership log has a fixed per-thread budget. Exceeding it must
// diagnose the budget, while cleanup permits reuse of the same mapped slots.
// The default budget is capped by the ID space, as are explicit overrides.
// RUN: %clang_ptsan -O2 -DEXPECT_CAPACITY=65536 %s -o %t.default
// RUN: %run %t.default
// RUN: %clang_ptsan_id32 -O2 -DEXPECT_CAPACITY=1048576 %s -o %t.default32
// RUN: %run %t.default32
// RUN: not %clang_ptsan -O2 -mllvm -ptsan-stack-id-log-capacity=65537 -c %s -o %t.o 2>&1 | FileCheck %s --check-prefix=LIMIT
// RUN: not %clang_ptsan -O2 -mllvm -ptsan-lam -mllvm -ptsan-stack-id-log-capacity=32769 -c %s -o %t.o 2>&1 | FileCheck %s --check-prefix=LIMIT
// LIMIT: PTSan stack ID log capacity cannot exceed the ID space
// RUN: %clang_ptsan -O2 -mllvm -ptsan-stack-id-log-capacity=16 %s -o %t.small
// RUN: not %run %t.small 2>&1 | FileCheck %s
// RUN: %clang_ptsan -O2 -mllvm -ptsan-stack-id-log-capacity=64 %s -o %t
// RUN: %run %t
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-stack-id-log-capacity=16 %s -o %t.small32
// RUN: not %run %t.small32 2>&1 | FileCheck %s
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-stack-id-log-capacity=64 %s -o %t.id32
// RUN: %run %t.id32
// CHECK: PTSan: per-thread stack ID log exhausted (16 entries).
// CHECK: Rebuild with -mllvm -ptsan-stack-id-log-capacity=<larger value> and rerun.

#include "Inputs/ptsan_test_interface.h"
#include <assert.h>

extern void ptsan_get_stack_ids(PTSanID *, uint64_t);
extern void ptsan_free_stack_ids(uintptr_t);

__attribute__((disable_sanitizer_instrumentation))
int main(void) {
#ifdef EXPECT_CAPACITY
  extern unsigned ClStackIDLogCapacity;
  assert(ClStackIDLogCapacity == EXPECT_CAPACITY);
#endif
  PTSanID ids[32];
  uintptr_t cursor = ptsan_stack_id_log.cursor;
  for (unsigned i = 0; i != 100; ++i) {
    ptsan_get_stack_ids(ids, 32);
    assert(ptsan_stack_id_log.cursor == cursor + 32);
    ptsan_free_stack_ids(cursor);
    assert(ptsan_stack_id_log.cursor == cursor);
  }
  return 0;
}
