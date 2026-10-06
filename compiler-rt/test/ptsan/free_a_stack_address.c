// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: not ./%exe 2> out.txt
// RUN: cat out.txt | FileCheck %s

// CHECK: PTSan found bad free

#include <stdio.h>
#include <stdlib.h>

void __attribute__((noinline)) inner(char *buf) {
  printf("buf = %s\n", buf);
  free(buf);
}

void __attribute__((noinline)) outer() {
  char buf[20];
  for (int i = 0; i < 20; i++) {
    buf[i] = 'A' + (i % 26);
  }
  inner(buf);
}

int main(int argc, char *argv[]) {
  outer();

  return 0;
}