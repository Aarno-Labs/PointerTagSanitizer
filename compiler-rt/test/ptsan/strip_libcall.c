// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: split-file %s %t
// RUN: %clang_raw -O2 -mllvm -ptsan-strip-ids -c %t/prefix.c -o %t/prefix.o
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error %t/main.c %t/prefix.o -o %t/test
// RUN: %t/test
// RUN: not %t/test freed 2>&1 | FileCheck %s
// CHECK: Found bad {{.*}} access

// A constant-length comparison can become a bcmp call, then expand into a
// load during codegen, after stripping. Keep the call so the runtime wrapper
// handles the tagged argument and still diagnoses invalid accesses.
//--- prefix.c
int has_prefix(const char *p) {
  return __builtin_memcmp(p, "_Z", 2) == 0;
}

//--- main.c
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

extern int has_prefix(const char *);

int main(int argc, char **argv) {
  char *p = malloc(3);
  assert(p && ((uintptr_t)p >> 48));
  p[0] = '_';
  p[1] = 'Z';
  p[2] = '\0';
  assert(has_prefix(p));
  free(p);
  if (argc > 1) {
    has_prefix(p);
    abort(); // The wrapper must report the access before this point.
  }
  return 0;
}
