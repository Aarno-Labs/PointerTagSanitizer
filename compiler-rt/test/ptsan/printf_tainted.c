// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe | FileCheck %s

// CHECK: malloc id is 0x1
// CHECK: Printing buf as first argument: ABCDEFGHIJKLMNOPQRS
// CHECK: Printing buf as varag argument: ABCDEFGHIJKLMNOPQRS

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

void __attribute__((noinline)) inner(int idx, char *buf) {
  char c = buf[idx];
  printf("buf[%d] = %c\n", idx, c);
}

void __attribute__((noinline)) outer(int idx) {
  char *buf = malloc(20);
  for (int i = 0; i < 19; i++) {
    buf[i] = 'A' + (i % 26);
  }
  buf[19] = '\0';

  printf("malloc id is 0x%lx\n", (uint64_t)buf>>48);

  inner(idx, buf);

  printf("Printing buf as first argument: ");
  printf(buf);
  printf("\n");

  printf("Printing buf as varag argument:");
  printf(" %s\n", buf);

  free(buf);
}

int main(int argc, char *argv[]) {
  outer(10);

  return 0;
}