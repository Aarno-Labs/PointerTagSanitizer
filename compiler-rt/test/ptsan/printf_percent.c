// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe | FileCheck %s

// CHECK: Loop is 0.84 % of program

#include "stdio.h"

__attribute__((noinline))
void func(double n) {
  printf("Loop is %0.3g %% of program\n", n);
}

int main() {
  func(84.0/100.0);
}