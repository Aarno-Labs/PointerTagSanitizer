// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o strong %s
// RUN: not ./strong 2> strong.err
// RUN: FileCheck %s --check-prefix=STRONG < strong.err
// RUN: %clang_ptsan -g -O2 -mllvm -ptsan-stack-temporal-safety=0 -o weak %s
// RUN: ./weak 2> weak.err
// RUN: FileCheck %s --check-prefix=WEAK < weak.err
// RUN: %clang_ptsan -g -O2 -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-no-stack-id-fast-path -o slow %s
// RUN: not ./slow 2> slow.err
// RUN: FileCheck %s --check-prefix=STRONG < slow.err

// RUN: %clang_ptsan_id32 -g -O2 -o strong %s
// RUN: not ./strong 2> strong.err
// RUN: FileCheck %s --check-prefix=STRONG < strong.err
// RUN: %clang_ptsan_id32 -g -O2 -mllvm -ptsan-stack-temporal-safety=0 -o weak %s
// RUN: ./weak 2> weak.err
// RUN: FileCheck %s --check-prefix=WEAK < weak.err
// RUN: %clang_ptsan_id32 -g -O2 -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-no-stack-id-fast-path -o slow %s
// RUN: not ./slow 2> slow.err
// RUN: FileCheck %s --check-prefix=STRONG < slow.err

// Disabling strong stack codegen permits the fast path to skip retirement.
// Runtime cleanup must still invalidate escaped pointers on the slow path.

// STRONG: Found bad POINT access
// WEAK-NOT: Found bad {{.*}} access

#include <stdio.h>

static char *Escaped;
static volatile char Sink;

__attribute__((noinline))
static void escape_stack(void) {
  char buf[16];
  buf[0] = 's';
  Escaped = buf;
}

__attribute__((noinline))
static void use_escaped(void) {
  Sink = Escaped[0];
}

int main(void) {
  escape_stack();
  use_escaped();
  fprintf(stderr, "sink=%c\n", Sink);
  return 0;
}
