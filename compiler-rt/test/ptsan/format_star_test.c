// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe > out.txt
// RUN: cat out.txt | FileCheck %s

//CHECK:   123 alpha
//CHECK:      xy beta
//CHECK:  42 gamma

#include <stdio.h>

#define NOINLINE __attribute__((noinline))

static NOINLINE void run(void) {
  const char *fmt = "\n%*u %s\n%*s %s\n%*u %s\n";

  int w1 = 6;
  unsigned u1 = 123u;
  const char *s1 = "alpha";

  int w2 = 8;
  const char *s2a = "xy";
  const char *s2b = "beta";

  int w3 = 4;
  unsigned u3 = 42u;
  const char *s3 = "gamma";

  printf(fmt, w1, u1, s1,   // \n%*u %s\n
               w2, s2a, s2b, // %*s %s\n
               w3, u3, s3);  // %*u %s\n
}

int main(void) {
  run();
  return 0;
}
