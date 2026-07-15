// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 83 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=BAD
// RUN: ./%exe 81 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=BAD
// RUN: ./%exe 80 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=GOOD
// RUN: ./%exe 79 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=GOOD

// BAD: Found bad RANGE access

// GOOD-NOT: Found bad RANGE access

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>

void __attribute((noinline)) inner(int *dest, int *src, int sz) {
  printf("in test, src id is 0x%x\n", (uint64_t)src >> 48);
  memcpy(dest, src, sz);
}

void __attribute__((noinline)) outer(int sz) {
  int bufA[20];
  int *bufB = (int *)malloc(20 * sizeof(int));
  for (int i = 0; i < 20; i++) {
    bufB[i] = i;
  }

  inner(bufA, bufB, sz);

  printf("Done: %d\n", bufA[10]);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <index>\n", argv[0]);
    return 1;
  }

  int sz = atoi(argv[1]);

  outer(sz);
}
