//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: %clang_ptsan -O2 -mllvm -ptsan-stop-after=discovery -S -emit-llvm \
// RUN:   -o %t.discovery.ll %s
// RUN: FileCheck %s --check-prefix=DISCOVERY < %t.discovery.ll
// RUN: %clang_raw -O2 -fno-discard-value-names -S -emit-llvm -o %t.input.ll %s
// RUN: opt -passes=ptsan-id-strip -S %t.input.ll -o %t.strip.ll
// RUN: FileCheck %s --check-prefix=STRIP < %t.strip.ll
// RUN: %clang_ptsan -O2 -mllvm -ptsan-abort-on-error -o %t %s
// RUN: %t c 256 256 2>&1 | FileCheck %s --check-prefix=GOOD
// RUN: %t s 256 256 2>&1 | FileCheck %s --check-prefix=GOOD
// RUN: not %t c 255 256 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not %t c 256 255 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not %t s 255 256 2>&1 | FileCheck %s --check-prefix=BAD

// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-abort-on-error -o %t %s
// RUN: %t c 256 256 2>&1 | FileCheck %s --check-prefix=GOOD
// RUN: %t s 256 256 2>&1 | FileCheck %s --check-prefix=GOOD
// RUN: not %t c 255 256 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not %t c 256 255 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not %t s 255 256 2>&1 | FileCheck %s --check-prefix=BAD

// Inline memcpy/memset need the same range checks and stripped operands as
// their ordinary counterparts, while retaining the inline intrinsic. Exercise
// both the full sanitizer and the standalone ID-strip pass. Runtime cases
// check successful copy/fill and independently undersized source/destination.

// DISCOVERY-LABEL: define {{.*}} @inline_copy(
// DISCOVERY-COUNT-2: call void (...) @__ptsan_access({{.*}}i64 256,
// DISCOVERY: call void @llvm.memcpy.inline.
// DISCOVERY-LABEL: define {{.*}} @inline_set(
// DISCOVERY: call void (...) @__ptsan_access({{.*}}i64 256,
// DISCOVERY: call void @llvm.memset.inline.

// STRIP-LABEL: define {{.*}} @inline_copy(
// STRIP: [[DST_BITS:%.*]] = ptrtoint ptr %dst to i64
// STRIP: [[DST_RAW:%.*]] = call i64 asm {{.*}}(i64 [[DST_BITS]], i64 281474976710655)
// STRIP: [[DST:%.*]] = inttoptr i64 [[DST_RAW]] to ptr
// STRIP: [[SRC_BITS:%.*]] = ptrtoint ptr %src to i64
// STRIP: [[SRC_RAW:%.*]] = call i64 asm {{.*}}(i64 [[SRC_BITS]], i64 281474976710655)
// STRIP: [[SRC:%.*]] = inttoptr i64 [[SRC_RAW]] to ptr
// STRIP: call void @llvm.memcpy.inline.p0.p0.i64(ptr{{[^,]*}} [[DST]], ptr{{[^,]*}} [[SRC]], i64 256, i1 false)
// STRIP-LABEL: define {{.*}} @inline_set(
// STRIP: [[SET_BITS:%.*]] = ptrtoint ptr %dst to i64
// STRIP: [[SET_RAW:%.*]] = call i64 asm {{.*}}(i64 [[SET_BITS]], i64 281474976710655)
// STRIP: [[SET_DST:%.*]] = inttoptr i64 [[SET_RAW]] to ptr
// STRIP: call void @llvm.memset.inline.p0.i64(ptr{{[^,]*}} [[SET_DST]], i8 7, i64 256, i1 false)

// GOOD-NOT: Found bad
// GOOD: inline-memory-ok
// GOOD-NOT: Found bad
// BAD: Found bad RANGE access

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__attribute__((noinline)) void inline_copy(char *dst, const char *src) {
  __builtin_memcpy_inline(dst, src, 256);
}

__attribute__((noinline)) void inline_set(char *dst) {
  __builtin_memset_inline(dst, 7, 256);
}

int main(int argc, char **argv) {
  assert(argc == 4);
  size_t dst_size = strtoul(argv[2], NULL, 10);
  size_t src_size = strtoul(argv[3], NULL, 10);
  char *dst = malloc(dst_size);
  char *src = malloc(src_size);
  assert(dst && src);
  memset(src, 3, src_size);

  int copy = argv[1][0] == 'c';
  if (copy) {
    inline_copy(dst, src);
  } else {
    inline_set(dst);
  }
  for (size_t i = 0; i < 256; ++i) {
    assert(dst[i] == (copy ? 3 : 7));
  }
  free(src);
  free(dst);
  puts("inline-memory-ok");
  return 0;
}
