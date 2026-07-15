// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe > out.txt
// RUN: cat out.txt | FileCheck %s

// CHECK: g_add[2]=3

// atomicrmw_array_demo.c
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#define NOINLINE __attribute__((noinline))

static _Atomic int g_add[4]  = {0, 0, 0, 0};
static _Atomic int g_xchg[4] = {10, 20, 30, 40};

NOINLINE void bump_idx(int idx) {
  (void)atomic_fetch_add_explicit(&g_add[idx], 1, memory_order_seq_cst);
}

NOINLINE int swap_idx(int idx, int v) {
  return atomic_exchange_explicit(&g_xchg[idx], v, memory_order_seq_cst);
}

NOINLINE int load_add(int idx) {
  return atomic_load_explicit(&g_add[idx], memory_order_relaxed);
}

NOINLINE int load_xchg(int idx) {
  return atomic_load_explicit(&g_xchg[idx], memory_order_relaxed);
}

NOINLINE int main(void) {
  for (int i = 0; i < 3; i++) {
    bump_idx(2);  /* generates: atomicrmw add ptr (g_add+2), i32 1 */
  }

  int old = swap_idx(1, 99); /* generates: atomicrmw xchg ptr (g_xchg+1), i32 99 */

  printf("g_add[2]=%d (expected 3), old_xchg=%d new_xchg=%d\n",
         load_add(2), old, load_xchg(1));
  return 0;
}
