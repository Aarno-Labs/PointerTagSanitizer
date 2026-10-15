//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan -fno-builtin-strstr -o %t %s
// RUN: %run %t

#include "Inputs/ptsan_test_interface.h"

#include <assert.h>
#include <string.h>

static const char Haystack[] = "shared constant haystack";
static const char Needle[] = "constant";

__attribute__((noinline)) static const char *findNeedle(const char *haystack,
                                                        const char *needle) {
  return strstr(haystack, needle);
}

int main(void) {
  assert(get_id(Haystack) != 0);
  assert(get_id(Haystack) != ptsan_default_id());
  assert(get_id(Haystack) == get_id(Needle));

  const char *result = findNeedle(Haystack, Needle);
  assert(result != NULL);
  assert(get_id(result) == get_id(Haystack));
  assert(strcmp(result, "constant haystack") == 0);

  char local[] = "local constant haystack";
  result = findNeedle(local, Needle);
  assert(result != NULL);
  assert(get_id(result) == get_id(local));
  return 0;
}
