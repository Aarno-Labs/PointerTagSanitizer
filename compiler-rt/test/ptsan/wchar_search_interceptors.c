// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan -O2 -fno-builtin -mllvm -ptsan-abort-on-error %s -o %t.c
// RUN: %run %t.c
// RUN: %clangxx_ptsan -x c++ -O2 -fno-builtin -mllvm -ptsan-abort-on-error %s -o %t.cxx
// RUN: %run %t.cxx

// Wide-character searches must preserve the input allocation's ID on a match
// and return an untagged null on a miss, for both C and C++ callers.
#include "Inputs/ptsan_test_interface.h"
#include <assert.h>
#include <stdlib.h>
#include <wchar.h>

static void check_result(const wchar_t *result, const wchar_t *expected) {
  assert(result == expected);
  // Pointer equality alone does not compare PTSan IDs.
  assert(get_id(result) == get_id(expected));
  if (expected)
    assert(*result == *expected);
}

int main(void) {
  wchar_t *buffer = (wchar_t *)malloc(6 * sizeof(wchar_t));
  assert(buffer && get_id(buffer) != 0);
  buffer[0] = L'a';
  buffer[1] = L'b';
  buffer[2] = L'a';
  buffer[3] = L'c';
  buffer[4] = L'a';
  buffer[5] = L'\0';
  const wchar_t *text = buffer;

  check_result(wmemchr(buffer, L'b', 5), buffer + 1);
  check_result(wcschr(buffer, L'a'), buffer);
  check_result(wcsrchr(buffer, L'a'), buffer + 4);
  check_result(wcsstr(buffer, L"ac"), buffer + 2);
  check_result(wcspbrk(buffer, L"cx"), buffer + 3);

  check_result(wmemchr(text, L'b', 5), text + 1);
  check_result(wcschr(text, L'\0'), text + 5);
  check_result(wcsrchr(text, L'a'), text + 4);
  check_result(wcsstr(text, L"ac"), text + 2);
  check_result(wcspbrk(text, L"cx"), text + 3);

  check_result(wmemchr(text, L'z', 5), NULL);
  check_result(wcschr(text, L'z'), NULL);
  check_result(wcsrchr(text, L'z'), NULL);
  check_result(wcsstr(text, L"zz"), NULL);
  check_result(wcspbrk(text, L"z"), NULL);

  free(buffer);
  return 0;
}
