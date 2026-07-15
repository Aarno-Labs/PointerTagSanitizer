// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clangxx_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> err.txt
// RUN: cat err.txt | FileCheck %s

// CHECK-NOT: Found bad

#include <stdio.h>

struct Obj {
  int x;
  int y;

  __attribute__((noinline)) Obj() : x(123), y(456) {
    printf("Obj ctor\n");
  }

  __attribute__((noinline)) ~Obj() {
    // These accesses are valid during exception unwinding.
    printf("Obj dtor: x=%d y=%d sum=%d\n", x, y, x + y);
  }
};

__attribute__((noinline)) void g() {
  Obj obj;
  throw 42;
}

int main() {
  try {
    g();
  } catch (int v) {
    printf("caught %d\n", v);
  }
  return 0;
}