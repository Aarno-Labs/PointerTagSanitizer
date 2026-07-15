// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -I%crt_src/include -I%crt_src/lib -o %exe %s
// RUN: ./%exe 2> err.txt
// RUN: cat err.txt | FileCheck %s --check-prefix=ERR

// ERR-NOT: All user functions should be called with ptsan_in_libc = 0

#include <stdio.h>

__attribute__((noinline, constructor(200)))
static void later_ctor(void) {
  puts("later_ctor");
}

__attribute__((noinline, constructor(100)))
static void early_ctor(void) {
  puts("early_ctor");
}

__attribute__((noinline))
int main(void) {
  puts("main");
  return 0;
}
