// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s %S/Inputs/global_array_direct_access_defs.c
// RUN: ./%exe 0 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=GOOD
// RUN: ./%exe 19 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=GOOD
// RUN: ./%exe 127 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=GOOD
// RUN: ./%exe 128 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=BAD

// BAD: Found bad POINT access
// GOOD-NOT: Found bad POINT access

#include <stdio.h>
#include <stdlib.h>

// Match the mafft constants.c shape: this TU only sees the aggregate global as
// external, and ptsan has to use the proxy load for both the access and the ID.
extern int amino_n[128];

int __attribute__((noinline)) calcfreq_like(int idx) {
  return amino_n[idx];
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <index>\n", argv[0]);
    return 1;
  }

  int idx = atoi(argv[1]);
  printf("amino_n[%d] = %d\n", idx, calcfreq_like(idx));

  return 0;
}
