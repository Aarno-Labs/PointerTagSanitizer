//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: %clang_ptsan -O2 -mavx -mllvm -ptsan-stop-after=discovery -S -emit-llvm \
// RUN:   -o %t.discovery.ll %s
// RUN: FileCheck %s --check-prefix=DISCOVERY < %t.discovery.ll
// RUN: %clang_raw -O2 -mavx -fno-discard-value-names -S -emit-llvm -o %t.input.ll %s
// RUN: opt -passes=ptsan-id-strip -S %t.input.ll -o %t.strip.ll
// RUN: FileCheck %s --check-prefix=STRIP < %t.strip.ll
// RUN: %clang_ptsan -O2 -mavx -mllvm -ptsan-abort-on-error -o %t %s
// RUN: %t 16 16 0 2>&1 | FileCheck %s --check-prefix=GOOD
// RUN: %t 16 17 1 2>&1 | FileCheck %s --check-prefix=GOOD
// RUN: not %t 16 16 1 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: %t 32 32 0 2>&1 | FileCheck %s --check-prefix=GOOD
// RUN: %t 32 33 1 2>&1 | FileCheck %s --check-prefix=GOOD
// RUN: not %t 32 32 1 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: %t 0 1 0 2>&1 | FileCheck %s --check-prefix=GOOD

// LDDQU intrinsics need the same checks as ordinary 16/32-byte loads, despite
// remaining target-specific calls in IR. Check base and byte-offset loads,
// including accesses extending one byte past the allocation. CLFLUSH only
// needs its pointer stripped: flushing a one-byte allocation must not require
// ownership of the whole cache line. Also exercise the standalone strip pass.

// DISCOVERY-LABEL: define {{.*}} @load16(
// DISCOVERY: [[LOAD16:%.*]] = call ptr @__ptsan_strip(ptr %ptr
// DISCOVERY: call void (...) @__ptsan_access({{.*}}i64 16,
// DISCOVERY: call <16 x i8> @llvm.x86.sse3.ldu.dq(ptr [[LOAD16]])
// DISCOVERY-LABEL: define {{.*}} @load32(
// DISCOVERY: [[LOAD32:%.*]] = call ptr @__ptsan_strip(ptr %ptr
// DISCOVERY: call void (...) @__ptsan_access({{.*}}i64 32,
// DISCOVERY: call <32 x i8> @llvm.x86.avx.ldu.dq.256(ptr [[LOAD32]])
// DISCOVERY-LABEL: define {{.*}} @flush_line(
// DISCOVERY-NOT: @__ptsan_access
// DISCOVERY: [[FLUSH_PTR:%.*]] = call ptr @__ptsan_strip(ptr %ptr
// DISCOVERY-NOT: @__ptsan_access
// DISCOVERY: call void @llvm.x86.sse2.clflush(ptr [[FLUSH_PTR]])
// DISCOVERY-NOT: @__ptsan_access
// DISCOVERY: ret void

// STRIP-LABEL: define {{.*}} @load16(
// STRIP: [[BITS16:%.*]] = ptrtoint ptr %ptr to i64
// STRIP: [[RAW16:%.*]] = call i64 asm {{.*}}(i64 [[BITS16]], i64 281474976710655)
// STRIP: [[PTR16:%.*]] = inttoptr i64 [[RAW16]] to ptr
// STRIP: call <16 x i8> @llvm.x86.sse3.ldu.dq(ptr [[PTR16]])
// STRIP-LABEL: define {{.*}} @load32(
// STRIP: [[BITS32:%.*]] = ptrtoint ptr %ptr to i64
// STRIP: [[RAW32:%.*]] = call i64 asm {{.*}}(i64 [[BITS32]], i64 281474976710655)
// STRIP: [[PTR32:%.*]] = inttoptr i64 [[RAW32]] to ptr
// STRIP: call <32 x i8> @llvm.x86.avx.ldu.dq.256(ptr [[PTR32]])
// STRIP-LABEL: define {{.*}} @flush_line(
// STRIP: [[FLUSH_BITS:%.*]] = ptrtoint ptr %ptr to i64
// STRIP: [[FLUSH_RAW:%.*]] = call i64 asm {{.*}}(i64 [[FLUSH_BITS]], i64 281474976710655)
// STRIP: [[FLUSH:%.*]] = inttoptr i64 [[FLUSH_RAW]] to ptr
// STRIP: call void @llvm.x86.sse2.clflush(ptr [[FLUSH]])

// GOOD-NOT: Found bad
// GOOD: x86-memory-ok
// GOOD-NOT: Found bad
// BAD: Found bad POINT access

#include <assert.h>
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__attribute__((noinline)) int load16(const char *ptr) {
  return _mm_movemask_epi8(_mm_lddqu_si128((const __m128i_u *)ptr));
}

__attribute__((noinline)) int load32(const char *ptr) {
  return _mm256_movemask_ps(
      _mm256_castsi256_ps(_mm256_lddqu_si256((const __m256i_u *)ptr)));
}

__attribute__((noinline)) void flush_line(const char *ptr) {
  _mm_clflush(ptr);
}

int main(int argc, char **argv) {
  assert(argc == 4);
  int width = atoi(argv[1]);
  size_t size = strtoul(argv[2], NULL, 10);
  size_t offset = strtoul(argv[3], NULL, 10);
  char *ptr = malloc(size);
  assert(ptr);
  memset(ptr, 0x80, size);

  if (width == 16) {
    assert(load16(ptr + offset) == 0xffff);
  } else if (width == 32) {
    assert(load32(ptr + offset) == 0xff);
  } else {
    flush_line(ptr + offset);
  }
  free(ptr);
  puts("x86-memory-ok");
  return 0;
}
