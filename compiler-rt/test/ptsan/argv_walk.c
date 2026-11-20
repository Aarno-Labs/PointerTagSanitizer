// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 30 2> out.txt

// CHECK-NOT: Found bad


#include <stdio.h>
#include <stdlib.h>

void __attribute__((noinline)) f(char *c) {
  printf("Got `%s`\n", c);
}

int main(int argc, char *argv[]) {
  while (*argv != 0) {
    f(*argv);
    argv++;
  }

  return 0;
}