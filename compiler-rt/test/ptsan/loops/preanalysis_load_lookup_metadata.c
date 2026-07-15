// Loop preanalysis metadata may depend on an SSA load value, but loads are
// memory reads and must be recovered by exact value-id lookup rather than
// reconstructed from pointer/type operands.
//
// RUN: %clang_raw -fsanitize=ptsan -O3 -g -fno-discard-value-names \
// RUN:   -S -emit-llvm -o - %s | FileCheck %s \
// RUN:   --implicit-check-not='{{!}}"load"' \
// RUN:   --implicit-check-not=deserialized{{.}}load
//
// CHECK-LABEL: define {{.*}}@copy_loaded_bound(
// CHECK: load i32, ptr {{.*}}, align 4, {{.*}}!ptsan.value.id ![[LOAD_ID:[0-9]+]]
// CHECK: !{!"ptsan.loop.bounds"
// CHECK: !{!"lookup", ![[LOAD_ID]]}

#include <stdint.h>

unsigned sz;

__attribute__((noinline)) void copy_loaded_bound(uint8_t *dst, uint8_t *src) {
  unsigned local_sz = sz;
  for (unsigned i = 0; i < local_sz; ++i) {
    dst[i] = src[i];
  }
}
