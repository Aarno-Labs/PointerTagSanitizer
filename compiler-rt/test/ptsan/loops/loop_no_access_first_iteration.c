// XXX: Is this actually checking for the spec case that caused it?
// I think it might not be
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 20 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=GOOD
// RUN: ./%exe 21 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=BAD

// BAD: Found bad
// GOOD-NOT: Found bad

#include <stdio.h>
#include <stdlib.h>

void __attribute__((noinline)) func(char *buf, unsigned n) {
  int sum = 0;
  int i = 0;
  while(1) {
    int s = 0;
    if (i > 0) {
      if (i >= n) {
        s = buf[i - 1];
        break;
      }
    }

    sum = s + buf[i] + sum;
    i++;
  }
  printf("sum = %d\n", sum);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <index>\n", argv[0]);
    return 1;
  }

  char buf[20];
  for (int i = 0; i < 20; i++) {
    buf[i] = i;
  }

  int idx = atoi(argv[1]);
  func(buf, idx);

  return 0;
}