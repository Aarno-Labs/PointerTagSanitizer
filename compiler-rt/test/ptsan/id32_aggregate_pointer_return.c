//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_raw -O2 -fno-strict-overflow -fno-discard-value-names \
// RUN:   -S -emit-llvm -o - %s | FileCheck %s --check-prefix=RAW
// RUN: %clang_ptsan_id32 -O2 -fno-strict-overflow -I%crt_src/include -o %t %s
// RUN: %run %t

#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>

struct PointerResult {
  int *pointer;
  bool valid;
};

// -fno-strict-overflow deliberately keeps this as a non-inbounds GEP. ID32
// strips that GEP, so inserting its result into the aggregate must retag it.
__attribute__((noinline)) struct PointerResult retreat(int *pointer) {
  struct PointerResult result = {pointer - 1, true};
  return result;
}

// RAW-LABEL: define dso_local { ptr, i8 } @retreat(
// RAW: [[PREVIOUS:%.*]] = getelementptr i8, ptr %pointer, i64 -4
// RAW: [[WITH_POINTER:%.*]] = insertvalue { ptr, i8 } poison, ptr [[PREVIOUS]], 0
// RAW: ret { ptr, i8 }

int main(void) {
  int *base = malloc(4 * sizeof(*base));
  assert(base);

  unsigned id = get_id(base);
  struct PointerResult result = retreat(base + 2);
  assert(result.valid);
  assert(result.pointer == base + 1);

  assert(get_id(result.pointer) == id);

  free(base);
  return 0;
}
