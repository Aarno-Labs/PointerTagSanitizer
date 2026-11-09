// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %opt_ptsan -mtriple=%ptsan_target_triple -ptsan-reoptimize=0 %S/do_cmp.ll -o do_cmp.ptsan.ll
// RUN: %llc -filetype=obj do_cmp.ptsan.ll -o do_cmp.ptsan.o
// RUN: %clang_ptsan -g -O2 -o %exe %s do_cmp.ptsan.o
// RUN: ./%exe


// This signed comparison came from Perl serializing pointer bits. Its result
// depends on the encoding's sign bit, not on the decoded address. In particular,
// 32-bit IDs set the sign bit while ordinary 16-bit IDs leave it clear.

#include <assert.h>
#include <stdint.h>
#include "../Inputs/ptsan_test_interface.h"

static int g;

__attribute__((noinline))
void *get_p(void) {
  return &g;
}

extern int ptr_gt_m1(void *p);

__attribute__((noinline))
int main(void) {
  void *p = get_p();
  assert(get_id(p) != 0);
  int r = ptr_gt_m1(p);
  // The volatile integer load prevents LLVM from folding this independent
  // reference calculation back into another pointer comparison.
  volatile intptr_t bits = (intptr_t)p;
  assert(r == (bits > -1));
  return 0;
}
