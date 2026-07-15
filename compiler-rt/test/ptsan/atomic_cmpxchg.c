
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe > out.txt
// RUN: cat out.txt | FileCheck %s

//CHECK: CAS idx=2 expect=2 -> ok=1 exp_after=2 cur=5
//CHECK: CAS idx=1 expect=0 -> ok=0 exp_after=1 cur=1


// atomic_cmpxchg_array_demo.c
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#define NOINLINE __attribute__((noinline))

static _Atomic int g_vals[4] = { 0, 1, 2, 3 };

/* Try a single compare-exchange on g_vals[idx].
   On failure, *expect_out is updated with the observed value (C11 semantics). */
NOINLINE int cas_try(int idx, int expect_in, int desired, int *expect_out) {
  _Atomic int *p = &g_vals[idx];
  int exp = expect_in;
  int ok = atomic_compare_exchange_strong_explicit(
      p, &exp, desired,
      memory_order_acq_rel,   /* success */
      memory_order_acquire);  /* failure */
  *expect_out = exp;
  return ok;
}

NOINLINE int load_at(int idx) {
  return atomic_load_explicit(&g_vals[idx], memory_order_relaxed);
}

int main(void) {
  int exp, ok;

  /* Success case: g_vals[2] is 2, so CAS(2 -> 5) succeeds. */
  exp = 2;
  ok = cas_try(2, exp, 5, &exp);
  printf("CAS idx=2 expect=2 -> ok=%d exp_after=%d cur=%d\n", ok, exp, load_at(2));

  /* Failure case: g_vals[1] is 1, so CAS(expect=0 -> 9) fails; exp gets set to 1. */
  exp = 0;
  ok = cas_try(1, exp, 9, &exp);
  printf("CAS idx=1 expect=0 -> ok=%d exp_after=%d cur=%d\n", ok, exp, load_at(1));

  return 0;
}
