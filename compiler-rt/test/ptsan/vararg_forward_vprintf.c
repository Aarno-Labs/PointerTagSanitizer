// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> err.txt 1> out.txt
// RUN: cat err.txt | FileCheck %s --check-prefix=ERR
// RUN: cat out.txt | FileCheck %s

// Regression test (minimized from 531.deepsjeng_r utils.cpp: myprintf).
//
// A variadic wrapper forwards its tagged va_list to the uninstrumented vprintf
// interceptor. The interceptor must strip the address of the va_list storage
// before va_copy or va_arg accesses it. This is particularly subtle on
// AArch64, where va_list is a struct passed indirectly through that address.

// ERR: Unreclaimed IDs at exit: 0
// CHECK: hello 42 world sum=100
// CHECK: OK

#include <stdarg.h>
#include <stdio.h>

__attribute__((noinline))
static void myprintf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap); // forward instrumented va_list -> uninstrumented libc
  va_end(ap);
}

int main(void) {
  myprintf("hello %d world sum=%d\n", 42, 100);
  puts("OK");
  return 0;
}
