// RUN: %clang_raw -fsanitize=ptsan -O1 -g -fno-discard-value-names \
// RUN:   -mllvm -ptsan-reoptimize=0 -o %exe %s
// RUN: %run %exe | FileCheck %s
// XFAIL: *
//
// This documents an unsupported false-positive shape. Once the all-ones
// sentinel is hidden behind opaque runtime values, strip lowering deliberately
// does not add dynamic checks to preserve it.

#include <stdint.h>
#include <stdio.h>

static void *volatile Sentinel = (void *)(uintptr_t)-1;
static void *volatile AddrMaskValue =
    (void *)(uintptr_t)0x0000ffffffffffffULL;

__attribute__((noinline)) int same_address_bits(void *a, void *b) {
  uintptr_t ai = (uintptr_t)a;
  uintptr_t bi = (uintptr_t)b;
  return ai == bi;
}

int main(void) {
  void *a = Sentinel;
  void *b = AddrMaskValue;

  if (same_address_bits(a, b)) {
    puts("bad");
    return 1;
  }

  puts("ok");
  return 0;
}

// CHECK: ok
