//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// A strip-only library needs raw addresses for its own accesses, but must
// preserve the caller's pointer identity when returning a derived pointer or
// passing it to an instrumented callback. Check equality and accesses on both
// paths across separately compiled translation units.
// RUN: split-file %s %t.dir
// RUN: %clang_strip -O2 -c %t.dir/helper.c -o %t.helper.o
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error %t.dir/main.c %t.helper.o -o %t
// RUN: %run %t

// RUN: %clang_strip -fsanitize-ptsan-32-bit-ids -DPTSAN_TEST_32_BIT_IDS -O2 -c %t.dir/helper.c -o %t.helper.o
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-abort-on-error %t.dir/main.c %t.helper.o -o %t
// RUN: %run %t

//--- helper.c
char *visit(char *base, unsigned index, void (*callback)(char *)) {
  char *element = base + index;
  *element = 42;
  callback(element);
  return element;
}

//--- main.c
#include <assert.h>
#include <stdlib.h>

char *visit(char *, unsigned, void (*)(char *));
static char *expected;

void check(char *element) {
  assert(element == expected);
  assert(*(volatile char *)element == 42);
  *(volatile char *)element = 7;
}

int main(void) {
  char *object = malloc(16);
  assert(object != NULL);
  expected = object + 3;
  char *returned = visit(object, 3, check);
  assert(returned == expected);
  assert(*(volatile char *)returned == 7);
  free(object);
  return 0;
}
