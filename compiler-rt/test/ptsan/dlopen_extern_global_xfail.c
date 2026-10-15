//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// XFAIL: *
// RUN: split-file %s %t
// RUN: %clang_ptsan -O2 -fPIC -shared %t/library.c -o %t/library.so
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error -rdynamic %t/main.c %t/access.c -ldl -o %t/test
// RUN: %t/test %t/library.so
// REQUIRES: x86_64-target-arch

// The library's weak extern proxy resolves to the executable's proxy. Its
// extern initializer must not replace the existing tagged pointer with a raw
// address: the executable's tagging constructor will not run again.

//--- library.c
extern int values[];

int *library_values(void) {
  return values;
}

//--- access.c
int values[4] = {3, 7, 11, 13};

int *executable_values(void) {
  return values;
}

//--- main.c
#include <assert.h>
#include <dlfcn.h>
#include <stdint.h>

int *executable_values(void);

int main(int argc, char **argv) {
  assert(argc == 2);
  unsigned before = (uintptr_t)executable_values() >> 48;
  assert(before && before != 0xffff);
  void *library = dlopen(argv[1], RTLD_NOW);
  assert(library);
  int *(*library_values)(void) = (int *(*)(void))dlsym(library, "library_values");
  assert(library_values);
  unsigned after = (uintptr_t)executable_values() >> 48;
  assert(after == before);
  assert(((uintptr_t)library_values() >> 48) == before);
  return 0;
}
