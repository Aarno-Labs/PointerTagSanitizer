// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> err.txt 1> out.txt
// RUN: cat err.txt | FileCheck %s --check-prefix=ERR
// RUN: cat out.txt | FileCheck %s

// Regression test (minimized from 531.deepsjeng_r utils.cpp: myprintf).
//
// A variadic wrapper forwards its va_list to a *libc* variadic function
// (vprintf). On AArch64 va_list is a struct passed by reference, so clang
// materializes a temporary copy to pass it. PTSan discovery only recognized
// va_list allocas fed to va_start, so it tagged that copy and handed a tagged
// pointer to uninstrumented vfprintf -- which faults in its prologue under
// software masking (no hardware TBI/LAM). Unlike vararg.c, the va_list is
// forwarded to *uninstrumented* libc, which is what exposes the bug.
//
// Fix: PTSanDiscovery.cpp isVaList() also recognizes va_list allocas by
// type, so the copy is left untagged.

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
