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

// RUN: %clang_ptsan -g -O2 -S -emit-llvm -o %exe.ll %s
// RUN: cat %exe.ll | FileCheck %s --check-prefix=IR


// BAD: Found bad COMPOUND access
// GOOD-NOT: Found bad COMPOUND access

// IR-LABEL: define dso_local void @inner
// IR: @ptsan_complain(i32 0,
// IR-NOT: @ptsan_complain(i32 1,
// IR: define dso_local

#include <stdio.h>
#include <stdlib.h>

void __attribute__((noinline)) inner(int idx, char *buf) {
  char c = buf[idx];
  if (c > 4) {
    buf[idx] = 0;
  }
  printf("buf[%d] = %c\n", idx, c);
}

void __attribute__((noinline)) outer(int idx) {
  char buf[20];
  for (int i = 0; i < 20; i++) {
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