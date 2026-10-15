//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe | FileCheck %s

// CHECK: pass-by-value ID is fresh and has size 2048

#include "Inputs/ptsan_test_interface.h"

#include <assert.h>

typedef struct {
  char payload[2048];
} large_blob_t;

__attribute__((noinline))
static void inspect_copy(large_blob_t blob, uint16_t caller_id) {
  uint16_t copy_id = get_id(blob.payload);

  assert(copy_id != 0);
  assert(copy_id != ptsan_default_id());
  assert(copy_id != caller_id);
  assert(ptsan_get_size(copy_id) == sizeof(blob));

  printf("pass-by-value ID is fresh and has size %llu\n",
         (unsigned long long)ptsan_get_size(copy_id));
}

int main(void) {
  large_blob_t blob = {0};
  uint16_t caller_id = get_id(blob.payload);

  assert(caller_id != 0);
  assert(caller_id != ptsan_default_id());
  inspect_copy(blob, caller_id);
  return 0;
}
