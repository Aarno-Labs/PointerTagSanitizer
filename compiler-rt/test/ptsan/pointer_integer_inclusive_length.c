//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: split-file %s %t.dir
// RUN: %clang_ptsan -O2 -mllvm -ptsan-clear-ids-on-arith=1 \
// RUN:   -I%S \
// RUN:   %t.dir/length.c %t.dir/main.c -o %t
// RUN: %run %t

// Perl's $0 buffer length is end - begin + 1, narrowed to 32 bits. LLVM
// reassociates this to (ptrtoint(end) + 1) - ptrtoint(begin), hiding one
// ptrtoint from the immediate pointer-subtraction recognizer. With different
// IDs, 16-bit tags disappear on truncation, but 32-bit-ID origins do not.
// Use unsigned integer subtraction to avoid undefined cross-object pointer
// subtraction. No cross-object memory access is needed to expose the bug.

//--- length.c
#include <stdint.h>

volatile uintptr_t saved_end;

uint32_t inclusive_length(const char *begin, const char *end) {
  uintptr_t adjusted_end = (uintptr_t)end + 1;
  // Keep the reassociated form from simplifying back to a direct subtraction.
  saved_end = adjusted_end;
  return (uint32_t)(adjusted_end - (uintptr_t)begin);
}

//--- main.c
#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stdlib.h>

uint32_t inclusive_length(const char *begin, const char *end);

int main(void) {
  char *first = malloc(16);
  char *second = malloc(16);
  assert(first && second);
  assert(get_id(first) != 0 && get_id(first) != ptsan_default_id());
  assert(get_id(second) != 0 && get_id(second) != ptsan_default_id());
  assert(get_id(first) != get_id(second));

  // Control: origins cancel when both endpoints have the same ID.
  assert(inclusive_length(first, first + 15) == 16);

  uint32_t expected =
      (uint32_t)(ptsan_get_address(second) + 16 - ptsan_get_address(first));
  assert(inclusive_length(first, second + 15) == expected);

  free(second);
  free(first);
  return 0;
}
