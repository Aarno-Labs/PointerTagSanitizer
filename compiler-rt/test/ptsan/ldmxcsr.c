// REQUIRES: x86_64-target-arch
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe > out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=OUT

// OUT: mxcsr before:
// OUT: mxcsr after :
// OUT-NOT: ERROR

// mxcsr_ld_st_test_barrier.c
#include <stdio.h>
#include <xmmintrin.h>

__attribute__((noinline))
static void compiler_barrier(void) {
  __asm__ volatile("" ::: "memory");
}

__attribute__((noinline))
static unsigned read_mxcsr(void) {
  // Usually lowers to llvm.x86.sse.stmxcsr(ptr) + load.
  compiler_barrier();
  unsigned v = _mm_getcsr();
  compiler_barrier();
  return v;
}

__attribute__((noinline))
static void write_mxcsr(unsigned x) {
  // Usually lowers to llvm.x86.sse.ldmxcsr(ptr).
  compiler_barrier();
  _mm_setcsr(x);
  compiler_barrier();
}

__attribute__((noinline))
static unsigned toggle_ftz(unsigned x) {
  // FTZ bit is 15.
  return x ^ (1u << 15);
}

__attribute__((noinline))
static void store_mxcsr_to_ptr(volatile unsigned *out) {
  compiler_barrier();
  *out = read_mxcsr();
  compiler_barrier();
}

__attribute__((noinline))
int main(void) {
  volatile unsigned stored0 = 0;
  volatile unsigned stored1 = 0;

  unsigned csr0 = read_mxcsr();
  store_mxcsr_to_ptr(&stored0);

  unsigned csr1 = toggle_ftz(csr0);

  printf("mxcsr before: 0x%08x (stored: 0x%08x)\n", csr0, (unsigned)stored0);

  // Exercise ldmxcsr
  write_mxcsr(csr1);

  unsigned csr2 = read_mxcsr();
  store_mxcsr_to_ptr(&stored1);

  printf("mxcsr after : 0x%08x (stored: 0x%08x)\n", csr2, (unsigned)stored1);

  // Restore original state.
  write_mxcsr(csr0);

  if (csr2 != csr1 || stored1 != csr1) {
    printf("ERROR: expected 0x%08x, got csr=0x%08x stored=0x%08x\n",
           csr1, csr2, (unsigned)stored1);
    return 1;
  }

  return 0;
}
