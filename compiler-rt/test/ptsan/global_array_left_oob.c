// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -mllvm -ptsan-abort-on-error -o %exe %s
// RUN: not ./%exe 2> err.txt
// RUN: cat err.txt | FileCheck %s

// CHECK: Found bad POINT access
// CHECK: Alloc size  0x1203
// CHECK: Access size 0x1
// CHECK: Min-Base = -71

#include <stdio.h>

#define FIRST_PSEUDO_REGISTER 53
#define MAX_MACHINE_MODE 87

unsigned char hard_regno_nregs[FIRST_PSEUDO_REGISTER][MAX_MACHINE_MODE];

__attribute__((noinline))
int read_like_gcc(int r, int mode) {
  return hard_regno_nregs[r][mode];
}

int main(void) {
  hard_regno_nregs[0][16] = 7;
  int value = read_like_gcc(-1, 16);
  printf("value=%d\n", value);
  return 0;
}
