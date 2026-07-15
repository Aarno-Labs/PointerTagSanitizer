// Loop preanalysis serializes the access range for buf[i + *off].  It must not
// recover that range by synthesizing a preheader load of *off: the loop mutates
// *off before the real access, so a synthesized preheader value would be stale
// and could incorrectly cover an out-of-bounds access.
//
// Without loop preanalysis, PTSan catches this as a normal point check.
//
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_raw -fsanitize=ptsan -O3 -g -fno-discard-value-names \
// RUN:   -mllvm -ptsan-runtime-validate \
// RUN:   -mllvm -ptsan-abort-on-error \
// RUN:   -mllvm -ptsan-stack-temporal-safety=0 \
// RUN:   -o %exe %s
// RUN: not --crash ./%exe 2> err
// RUN: FileCheck %s < err
//
// CHECK: Found bad POINT access

#include <stdio.h>
#include <stdlib.h>

volatile int g_trigger = 2;

__attribute__((noinline)) void mutate(int *p) { *p = 64; }

__attribute__((noinline)) int test(unsigned char *buf, int *off, int n,
                                   int trigger) {
  int sum = 0;
  for (int i = 0; i < n; ++i) {
    if (i == trigger) {
      mutate(off);
    }
    sum += buf[i + *off];
  }
  return sum;
}

int main(void) {
  int *off = malloc(sizeof(*off));
  *off = 0;

  unsigned char *buf = malloc(8);
  for (int i = 0; i < 8; ++i) {
    buf[i] = 1;
  }

  int sum = test(buf, off, 8, g_trigger);
  printf("%d\n", sum);
  return 0;
}
