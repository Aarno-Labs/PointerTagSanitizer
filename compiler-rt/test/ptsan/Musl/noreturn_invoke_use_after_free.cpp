//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsanxx_musl_strict %s -o %t
// RUN: not %run %t 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: %run %t safe 2>&1 | FileCheck %s --check-prefix=GOOD
// RUN: %ptsanxx_musl_id32_strict %s -o %t.id32
// RUN: not %run %t.id32 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: %run %t.id32 safe 2>&1 | FileCheck %s --check-prefix=GOOD
// BAD: Found bad POINT access
// GOOD-NOT: Found bad
// GOOD: cleanup read accepted

// noreturn forbids a normal return, not unwinding. The destructor must load
// current bounds after the invoke, rather than reuse bounds from before free.
#include <cstdio>
#include <cstdlib>

static volatile char sink;
struct Cleanup {
  char *p;
  ~Cleanup() { sink = *p; }
};

[[noreturn]] __attribute__((noinline))
void release_and_throw(char *p, bool release) {
  if (release)
    free(p);
  throw 1;
}

__attribute__((noinline)) void check_cleanup(char *p, bool release) {
  Cleanup cleanup{p};
  release_and_throw(p, release);
}

int main(int argc, char **) {
  char *p = static_cast<char *>(malloc(64));
  if (!p)
    return 2;
  *p = 42;
  bool release = argc == 1;
  try {
    check_cleanup(p, release);
  } catch (int) {
    puts("cleanup read accepted");
  }
  if (!release)
    free(p);
  return 0;
}
