//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise strchrnul after live allocations exhaust PTSan's precise ID space.
// The public API must preserve pointer provenance even when bounds fall back to
// the default ID.
//
// RUN: %ptsan_musl_nonstrict -Wall -Wextra -Werror -fno-builtin -o %t %s
// RUN: %run %t

#define _GNU_SOURCE

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum { AllocationCount = 70000 };

int main(void) {
  char **strings = (char **)calloc(AllocationCount, sizeof(char *));
  if (!strings) {
    return 1;
  }

  for (size_t index = 0; index < AllocationCount; ++index) {
    strings[index] = (char *)malloc(2);
    if (!strings[index]) {
      return 2;
    }
    strings[index][0] = 'A';
    strings[index][1] = '\0';

    char *end = strchrnul(strings[index], 'Z');
    if (end - strings[index] != 1) {
      return 3;
    }
  }

  for (size_t index = 0; index < AllocationCount; ++index) {
    free(strings[index]);
  }
  free(strings);
  return 0;
}
