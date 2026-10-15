// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %opt_ptsan -mtriple=%ptsan_target_triple -ptsan-reoptimize=0 %S/do_cmp.ll -o do_cmp.ptsan.ll
// RUN: %llc -filetype=obj do_cmp.ptsan.ll -o do_cmp.ptsan.o
// RUN: %clang_ptsan -g -O2 -o %exe %s do_cmp.ptsan.o
// RUN: ./%exe > out.txt
// RUN: cat out.txt | FileCheck %s


// CHECK: ptr > (void*)-1 => 1

#include <stdio.h>

static int g;

__attribute__((noinline))
void *get_p(void) {
  return &g;
}

extern int ptr_gt_m1(void *p);

__attribute__((noinline))
int main(void) {
  void *p = get_p();
  int r = ptr_gt_m1(p);
  printf("ptr > (void*)-1 => %d\n", r);
  // Uninstrumented: typically 1 (small nonnegative address > -1 in signed order).
  // With your pass masking the RHS to 48b, it likely flips to 0.
  return r == 0;  // return nonzero if the bug triggers
}