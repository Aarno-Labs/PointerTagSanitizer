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

// RUN: %clang_ptsan -g -O2 -o %exe.ll %s -S -emit-llvm
// RUN: cat %exe.ll | FileCheck %s --check-prefix=LL

// this should still work/run
// RUN: %clang_ptsan -g -O2 -mllvm -ptsan-disable -o %exe.ll %s
// RUN: ./%exe 19

// BAD: Found bad POINT access
// GOOD-NOT: Found bad POINT access

// LL-LABEL:define{{.*}}@outer
// LL-NOT: @ptsan_complain
// LL: ret void

#include <stdio.h>
#include <stdlib.h>

void __attribute__((noinline)) inner(int idx, char *buf) {
  char c = buf[idx];
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