// RUN: %clang_raw -fsanitize=ptsan -O2 -g -fno-discard-value-names -fno-vectorize -fno-slp-vectorize -fno-unroll-loops -mllvm -ptsan-reoptimize=0 -S -emit-llvm -o - %s | FileCheck %s
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_raw -fsanitize=ptsan -O2 -g -fno-discard-value-names -fno-vectorize -fno-slp-vectorize -fno-unroll-loops -mllvm -ptsan-runtime-validate -mllvm -ptsan-print-stats -o %exe %s
// RUN: ./%exe 4 2> good.err && FileCheck %s --check-prefixes=GOOD,STATS < good.err
// RUN: ./%exe 5 2> bad.err && FileCheck %s --check-prefixes=BAD,STATS < bad.err
// RUN: ./%exe 8 2> bad_large.err && FileCheck %s --check-prefixes=BAD,STATS < bad_large.err

// A single access inside nested loops can be covered by both loops when the
// pointer varies only with the outer induction variable. Prefer the outer
// preanalysis check so the runtime check executes once before the outer loop,
// not once per outer iteration before the inner loop. The inline asm keeps the
// inner loop real and always executing without adding another checked memory
// access.
//
// CHECK-LABEL: define {{.*}}@nested_outer_elision(
// CHECK: for.cond1.preheader.preheader
// CHECK: call void @ptsan_complain
// CHECK-NOT: call void @ptsan_complain
// CHECK: for.cond1.preheader:
// CHECK-NOT: call void @ptsan_complain
// CHECK: for.body4.preheader:
// CHECK-DAG: !{!"ptsan.loop.bounds", i1 true
// CHECK-DAG: !{!"ptsan.loop.bounds", i1 true
//
// GOOD-NOT: Found bad
// BAD: Found bad PREANALYZED_LOOP access
// STATS: Checks called:
// STATS: PREANALYZED_LOOP: 1

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

volatile uint8_t sink;

__attribute__((noinline))
void nested_outer_elision(uint8_t *buf, unsigned rows) {
  for (unsigned i = 0; i < rows; ++i) {
    for (unsigned j = 0; j < 4; ++j) {
      asm volatile("" :: "r"(j) : "memory");
      buf[i] = (uint8_t)j;
    }
  }
}

int main(int argc, char **argv) {
  unsigned rows = argc > 1 ? (unsigned)atoi(argv[1]) : 4;
  uint8_t *buf = (uint8_t *)malloc(4);
  if (!buf) {
    fprintf(stderr, "malloc failed\n");
    return 1;
  }

  for (unsigned i = 0; i < 4; ++i) {
    buf[i] = 0;
  }

  nested_outer_elision(buf, rows);
  sink = buf[0];

  free(buf);
  return 0;
}
