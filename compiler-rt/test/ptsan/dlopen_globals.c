//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: split-file %s %t
// RUN: %clang_ptsan -O2 -fPIC -shared %t/library.c -o %t/library.so
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error %t/main.c -ldl -o %t/test
// RUN: %t/test %t/library.so
// REQUIRES: x86_64-target-arch

// A dlopened module's constructor initializes its ordinary global proxies
// before dlopen returns. Check the actual pointer returned by instrumented code.

//--- library.c
int values[4] = {3, 7, 11, 13};

int *get_values(void) {
  return values;
}

//--- main.c
#include <assert.h>
#include <dlfcn.h>
#include <stdint.h>

int main(int argc, char **argv) {
  assert(argc == 2);
  void *library = dlopen(argv[1], RTLD_NOW);
  assert(library);
  int *(*get_values)(void) = (int *(*)(void))dlsym(library, "get_values");
  assert(get_values);
  int *values = get_values();
  unsigned id = (uintptr_t)values >> 48;
  assert(id && id != 0xffff);
  assert(values[0] == 3 && values[3] == 13);
  assert(((uintptr_t)get_values() >> 48) == id);
  return 0;
}
