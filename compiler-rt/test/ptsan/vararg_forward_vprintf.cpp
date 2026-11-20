// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clangxx_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> err.txt 1> out.txt
// RUN: cat err.txt | FileCheck %s --check-prefix=ERR
// RUN: cat out.txt | FileCheck %s

// C++ variant of vararg_forward_vprintf.c (minimized from 531.deepsjeng_r).
// This uses only <cstdio>/<cstdarg> (no STL), so it needs no instrumented C++
// standard library. On AArch64, C++ gives va_list a different source-level
// type from C, but both reach the interceptor through an indirectly passed
// address which must have its ID stripped before the va_list is accessed.

// ERR: Unreclaimed IDs at exit: 0
// CHECK: hello 42 world sum=100
// CHECK: OK

#include <cstdarg>
#include <cstdio>

__attribute__((noinline))
static void myprintf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap); // forward instrumented va_list -> uninstrumented libc
  va_end(ap);
}

int main() {
  myprintf("hello %d world sum=%d\n", 42, 100);
  puts("OK");
  return 0;
}
