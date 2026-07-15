// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 30 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=BAD
// RUN: ./%exe 21 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=BAD
// RUN: ./%exe 20 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=BAD
// RUN: ./%exe 19 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=GOOD

// BAD: Found bad COMPOUND access
// GOOD-NOT: Found bad COMPOUND access

#include <stdio.h>
#include <stdlib.h>

void __attribute__((noinline)) inner(int idx, char *buf) {
  char c1 = buf[idx];
  char c2 = buf[idx + 5];
  printf("buf[%d] = %c, buf[%d] = %c\n", idx, c1, idx + 5, c2);
}

void __attribute__((noinline)) outer(int idx) {
  char buf[25];
  for (int i = 0; i < 25; i++) {
    buf[i] = 'A' + (i % 26);
  }
  inner(idx, buf);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <index>\n", argv[0]);
    return 1;
  }

  int idx = atoi(argv[1]);
  outer(idx);

  return 0;
}