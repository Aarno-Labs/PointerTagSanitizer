//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: %clang_ptsan -O2 -fno-unroll-loops -mllvm -ptsan-preanalyze-loops=1 \
// RUN:   -mllvm -ptsan-stop-after=discovery -S -emit-llvm -o %t.ll %s
// RUN: FileCheck %s --check-prefix=IR < %t.ll
// RUN: %clang_ptsan -O2 -mllvm -ptsan-preanalyze-loops=1 \
// RUN:   -mllvm -ptsan-abort-on-error -o %t %s
// RUN: %t 2>&1 | FileCheck %s --check-prefix=GOOD
// RUN: not %t bad 2>&1 | FileCheck %s --check-prefix=BAD

// Reduced from Fleetbench's SIMD benchmark. CLFLUSH is a strip-only PtrOp:
// it has NO_CHECK and a null Size. Preanalysis must not try to build a range
// from it, but discovery must still strip its pointer. The one-byte allocation
// also ensures we don't accidentally require ownership of an entire cache line.
// The mixed loop writes one base and flushes a different one. The flush must
// not count as an unsolved base: its summary must remain complete and contain
// only the written base. The bad run still needs to catch an out-of-bounds write.

// IR-LABEL: define {{.*}} @flush_loop(
// IR-NOT: @__ptsan_access
// IR: [[RAW:%.*]] = call ptr @__ptsan_strip(ptr %{{.*}})
// IR-NOT: @__ptsan_access
// IR: call void @llvm.x86.sse2.clflush(ptr [[RAW]])
// IR-NOT: @__ptsan_access
// IR: }
// IR-LABEL: define {{.*}} @write_and_flush_loop(
// IR: call void @llvm.sideeffect() {{.*}}!ptsan.loop.bounds ![[BOUNDS:[0-9]+]]
// IR: call void (...) @__ptsan_access(
// IR: [[OTHER_RAW:%.*]] = call ptr @__ptsan_strip(ptr %flush_ptr
// IR: call void @llvm.x86.sse2.clflush(ptr [[OTHER_RAW]])
// IR: }
// IR: ![[BOUNDS]] = !{!"ptsan.loop.bounds", i1 true, !{{[0-9]+}}}
// GOOD-NOT: Found bad
// GOOD: clflush-loop-ok
// GOOD-NOT: Found bad
// BAD: Found bad {{.*}} access

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

__attribute__((noinline)) void flush_loop(char *ptr, unsigned count) {
  for (unsigned i = 0; i < count; ++i) {
    __builtin_ia32_clflush(ptr + i);
  }
}

__attribute__((noinline)) void write_and_flush_loop(char *ptr, char *flush_ptr,
                                                   unsigned count) {
  for (unsigned i = 0; i < count; ++i) {
    ptr[i] = 42;
    __builtin_ia32_clflush(flush_ptr);
  }
}

int main(int argc, char **argv) {
  char *ptr = malloc(1);
  char *flush_ptr = malloc(1);
  assert(ptr && flush_ptr);
  flush_loop(ptr, 1);
  write_and_flush_loop(ptr, flush_ptr, argc > 1 ? 2 : 1);
  assert(ptr[0] == 42);
  free(flush_ptr);
  free(ptr);
  puts("clflush-loop-ok");
  return 0;
}
