//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// XFAIL: *
// RUN: %clang_raw -O3 -o %t.raw %s
// RUN: %run %t.raw | FileCheck %s
// RUN: %clang_ptsan_id32 -O3 -o %t.ptsan %s
// RUN: %run %t.ptsan | FileCheck %s

// SPASS stores negative integer symbols in pointer-typed values and compares
// them as opaque sentinels. These distinct pointer values must compare unequal.
// ID32 instead treats the negative value as encoded: its high bits select an
// unused ID with zero decode bias, leaving only the low 32 bits for comparison.
// This aliases the two sentinels. Keep the intended result as an expected
// failure, even if this representation limitation is never addressed.

// CHECK: member: 0

#include <stdint.h>
#include <stdio.h>

struct Node {
  struct Node *next;
  void *value;
};

__attribute__((noinline)) int pointer_member(struct Node *list, void *element) {
  while (list) {
    if (element == list->value)
      return 1;
    list = list->next;
  }
  return 0;
}

int main(int argc, char **argv) {
  int32_t symbol = -505 - argc;
  void *negative = (void *)(intptr_t)symbol;
  void *low_bits = (void *)(uintptr_t)(uint32_t)symbol;
  struct Node node = {0, low_bits};

  printf("member: %d\n", pointer_member(&node, negative));
  return 0;
}
