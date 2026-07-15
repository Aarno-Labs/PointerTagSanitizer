// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clangxx_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> err.txt
// RUN: cat err.txt | FileCheck %s

// CHECK: Unreclaimed IDs at exit: 0
#include <stdio.h>

__attribute__((noinline)) void use_buf(const volatile char *p) {
  printf("buf[0]=%d\n", (int)p[0]);
}

__attribute__((noinline)) void g() {
  char buf[32];
  buf[0] = 17;
  buf[31] = 99;
  use_buf(buf);
  throw 42;
}

int main() {
  try {
    g();
    printf("should not get here\n");
  } catch (int x) {
    printf("caught %d\n", x);
  }

  printf("continued normally\n");
  return 0;
}