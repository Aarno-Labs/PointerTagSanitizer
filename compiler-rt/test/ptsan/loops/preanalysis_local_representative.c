// RUN: %clang_raw -fsanitize=ptsan -g -O2 -fno-vectorize -fno-slp-vectorize -fno-unroll-loops -mllvm -ptsan-stop-after=optimization -S -emit-llvm -o %t.ll %s
// RUN: FileCheck %s --implicit-check-not='!{!"ptsan.loop.bounds", i1 false' < %t.ll

// The inner access range depends on the already-computed row/end pointers and
// their masked integer delta. Preanalysis should serialize those dominating
// local IR representatives as captures, keeping the summary complete without
// instruction-ID lookups or rebuilding the underlying expressions.

// CHECK-LABEL: define {{.*}} @local_representative(
// CHECK-SAME: ptr {{[^%]*}}[[BUF:%[^, )]+]],
// CHECK: [[ROW:%[^ ]+]] = getelementptr {{.*}} ptr [[BUF]],
// CHECK: [[END:%[^ ]+]] = getelementptr {{.*}} ptr [[ROW]], i64 7
// CHECK: [[DELTA:%[^ ]+]] = sub
// CHECK: call void @llvm.sideeffect() [ "ptsan.loop.preheader"(i64 {{[0-9]+}}), "deopt"(ptr [[BUF]], ptr [[ROW]], ptr [[END]], i64 [[DELTA]]) ]{{.*}}!ptsan.loop.bounds ![[INNER_BOUNDS:[0-9]+]]
// CHECK-DAG: ![[INNER_BOUNDS]] = !{!"ptsan.loop.bounds", i1 true, ![[INNER_BASE:[0-9]+]]}
// CHECK-DAG: ![[INNER_BASE]] = !{!"base", ![[BASE_CAPTURE:[0-9]+]], ![[INNER_RANGE:[0-9]+]]}
// CHECK-DAG: ![[BASE_CAPTURE]] = !{!"capture", i32 0}
// CHECK-DAG: ![[INNER_RANGE]] = !{!"LoopAccessRange", ![[ROW_SCEV:[0-9]+]], ![[END_SCEV:[0-9]+]], ![[DELTA_SCEV:[0-9]+]], ![[SIZE_SCEV:[0-9]+]]}
// CHECK-DAG: ![[ROW_SCEV]] = !{!"scev", i16 15, ![[ROW_CAPTURE:[0-9]+]]}
// CHECK-DAG: ![[ROW_CAPTURE]] = !{!"capture", i32 1}
// CHECK-DAG: ![[END_SCEV]] = !{!"scev", i16 15, ![[END_CAPTURE:[0-9]+]]}
// CHECK-DAG: ![[END_CAPTURE]] = !{!"capture", i32 2}
// CHECK-DAG: ![[DELTA_SCEV]] = !{!"scev", i16 15, ![[DELTA_CAPTURE:[0-9]+]]}
// CHECK-DAG: ![[DELTA_CAPTURE]] = !{!"capture", i32 3}

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
