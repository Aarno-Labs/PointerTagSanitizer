// RUN: %clang_ptsan_plain -O1 -g -fno-discard-value-names \
// RUN:   -mllvm -ptsan-reoptimize=0 -o %t %s
// RUN: %run %t
// XFAIL: *
//
// 16-bit stripping loses the high bits of an opaque all-ones sentinel,
// making it equal to the address mask. 32-bit stripping preserves this
// particular sentinel; arbitrary integer sentinels remain unsupported.

#include <assert.h>
#include <stdint.h>

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

  assert(!same_address_bits(a, b));
  return 0;
}
