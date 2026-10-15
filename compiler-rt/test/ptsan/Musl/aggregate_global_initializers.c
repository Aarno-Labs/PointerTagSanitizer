//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: split-file %s %t
// RUN: %ptsan_musl_strict -O2 -mllvm -ptsan-abort-on-error %t/owners.c %t/main.c -o %t/test
// RUN: %t/test
// RUN: %ptsan_musl_strict -O2 -mllvm -ptsan-abort-on-error -mllvm -ptsan-no-opts %t/owners.c %t/main.c -o %t/noopts
// RUN: %t/noopts

// Keep the targets in the same module as their initializers, but read them
// from another module so the loads cannot be folded to direct global accesses.
//--- owners.c
int values[4] = {11, 22, 33, 44};
int *pointers[] = {values, values + 1};
struct Table { int *elements[2]; const char *name; };
const struct Table tables[] = {
    {{values, values + 2}, "first"},
    {{values + 1, values + 3}, "second"}};

//--- main.c
#include <assert.h>
#include <stdint.h>

extern int values[4];
extern int *pointers[];
struct Table { int *elements[2]; const char *name; };
extern const struct Table tables[];

int main(void) {
  uintptr_t id = (uintptr_t)values >> 48;
  assert(id && id != 0xffff);
  for (int i = 0; i < 2; ++i) {
    assert((uintptr_t)pointers[i] >> 48 == id);
    assert(*pointers[i] == values[i]);
    for (int j = 0; j < 2; ++j) {
      assert((uintptr_t)tables[i].elements[j] >> 48 == id);
      assert(*tables[i].elements[j] == values[i + 2 * j]);
    }
    uintptr_t name_id = (uintptr_t)tables[i].name >> 48;
    assert(name_id && name_id != 0xffff);
    assert(tables[i].name[0] == (i ? 's' : 'f'));
  }
  return 0;
}
