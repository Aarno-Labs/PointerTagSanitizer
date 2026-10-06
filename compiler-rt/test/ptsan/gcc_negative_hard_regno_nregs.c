// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -mllvm -ptsan-abort-on-error -o %exe %s
// RUN: not ./%exe 2> err.txt
// RUN: cat err.txt | FileCheck %s

// CHECK: Found bad POINT access
// CHECK: Alloc size  0x1203
// CHECK: Access size 0x1
// CHECK: Min-Base = -71

#include <stdio.h>
#include <stdlib.h>

#define FIRST_PSEUDO_REGISTER 53
#define MAX_MACHINE_MODE 87

volatile unsigned char hard_regno_nregs[FIRST_PSEUDO_REGISTER][MAX_MACHINE_MODE];
short *reg_renumber;
int ira_conflicts_p;
volatile int escaped;

__attribute__((noinline))
int pseudo_mode(int reg) {
  escaped += reg;
  return 16;
}

__attribute__((noinline))
int count_spilled_pseudo_like_gcc(int reg) {
  int r = reg_renumber[reg];
  int nregs = hard_regno_nregs[r][pseudo_mode(reg)];

  if (ira_conflicts_p && r < 0)
    return 0;

  return nregs;
}

int main(void) {
  reg_renumber = (short *)calloc(256, sizeof(short));
  if (!reg_renumber)
    return 1;

  ira_conflicts_p = 1;
  reg_renumber[165] = -1;
  hard_regno_nregs[0][16] = 7;

  int out = count_spilled_pseudo_like_gcc(165);
  printf("out=%d escaped=%d\n", out, escaped);
  free(reg_renumber);
  return 0;
}
