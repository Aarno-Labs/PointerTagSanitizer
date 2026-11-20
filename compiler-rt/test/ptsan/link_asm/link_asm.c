// XFAIL: *

// We don't support linking asm files. Here's a small test in case
// we ever decide to try to fix that!

// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s %S/lib/asm.S
// RUN: ./%exe 2> out.txt

#include <stdio.h>
#include <stdint.h>

extern int asm_sum_two(const int *p);  // defined in asm

int g_arr[8];

__attribute__((noinline))
void fill_squares(void) {
  for (int i = 0; i < 8; i++) {
    g_arr[i] = (i+1) * (i+1);
  }
}

__attribute__((noinline))
int main(void) {
  fill_squares();
  int s = asm_sum_two(g_arr);
  printf("sum first two = %d\n", s);  // with squares: 0 + 1 = 1
  return 0;
}
