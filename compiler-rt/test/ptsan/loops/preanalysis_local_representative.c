// RUN: %clang_raw -fsanitize=ptsan -g -O2 -fno-vectorize -fno-slp-vectorize -fno-unroll-loops -S -emit-llvm -o - %s | FileCheck %s --implicit-check-not="{{!\\{!\"ptsan.loop.bounds\", i1 false}}"

// CHECK: PREANALYZED_LOOP
// CHECK: !{!"ptsan.loop.bounds", i1 true
// CHECK: !{!"LoopAccessRange"
// CHECK: !{!"gep",
// CHECK: !{!"binop",

#include <stdint.h>

__attribute__((noinline))
void local_representative(uint8_t *buf, unsigned rows) {
  for (unsigned i = 0; i < rows; ++i) {
    uint8_t *row = &buf[i * 8];
    uint8_t *end = row + 7;
    uintptr_t first48 = (uintptr_t)row & 0x0000ffffffffffffULL;
    uintptr_t last48 = (uintptr_t)end & 0x0000ffffffffffffULL;
    uintptr_t delta = last48 - first48;

    // Keep the local representatives available for loop-bound serialization.
    asm volatile("" :: "r"(row), "r"(end), "r"(delta));

    for (unsigned j = 0; j < 8; ++j)
      row[j] = (uint8_t)j;
  }
}
