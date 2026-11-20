// Make sure we're handling 32-bit atomics correctly and not just
// assuming all accesses are 64-bit

// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> err.txt
// RUN: cat err.txt | FileCheck %s

// CHECK-NOT: Found bad

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#define NOINLINE __attribute__((noinline))

enum { INIT_STATUS_OK = 0x100u };

NOINLINE uint32_t fini_like(uint32_t start_count) {
  _Atomic uint32_t init_status = (INIT_STATUS_OK | start_count);

  /* Force the address to "escape" so the compiler can't scalar-replace it. */
  asm volatile("" : : "r"(&init_status) : "memory");

  uint32_t v;
  uint32_t nv;

  do {
    v = atomic_load_explicit(&init_status, memory_order_acquire);
    if (v == (INIT_STATUS_OK | 1u)) {
      nv = 1u;
    } else {
      nv = v - 1u;
    }
  } while (!atomic_compare_exchange_weak_explicit(&init_status,
                                                  &v, nv,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire));

  if (nv == 1u) {
    /* Stand-in for your cleanup sequence. */
    (void)atomic_fetch_sub_explicit(&init_status, 1u, memory_order_release);
  }

  return atomic_load_explicit(&init_status, memory_order_relaxed);
}

int main(void) {
  uint32_t out = fini_like(3u);
  printf("final init_status = 0x%08x\n", out);
  return 0;
}
