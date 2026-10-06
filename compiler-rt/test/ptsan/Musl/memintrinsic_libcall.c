//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -DSIZE=16 -o %t.small %s
// RUN: %run %t.small
// RUN: %ptsan_musl_strict -DSIZE=304 -o %t.large %s
// RUN: %run %t.large
// RUN: %run %t.large indirect
// RUN: %run %t.large builtin
// RUN: for mode in 1 2 3 4 5; do \
// RUN:   not %run %t.large direct $mode 2>&1 | \
// RUN:     FileCheck %s --check-prefix=BAD || exit 1; \
// RUN: done
// RUN: for mode in 1 2 3 4 5; do \
// RUN:   not %run %t.large indirect $mode 2>&1 | \
// RUN:     FileCheck %s --check-prefix=BAD || exit 1; \
// RUN: done
// RUN: for mode in 1 2 3 4 5; do \
// RUN:   not %run %t.large builtin $mode 2>&1 | \
// RUN:     FileCheck %s --check-prefix=INTRINSIC-BAD || exit 1; \
// RUN: done
// RUN: %ptsan_musl_strict -DSIZE=16 \
// RUN:   -S -emit-llvm -o - %s | \
// RUN:   FileCheck %s --check-prefix=SMALL-IR
// RUN: %ptsan_musl_strict -DSIZE=304 \
// RUN:   -S -emit-llvm -o - %s | \
// RUN:   FileCheck %s --check-prefix=LARGE-IR
// RUN: %ptsan_musl_strict -DSIZE=16 -S -o - %s | \
// RUN:   FileCheck %s --check-prefix=SMALL-ASM
// RUN: %ptsan_musl_strict -DSIZE=304 -S -o - %s | \
// RUN:   FileCheck %s --check-prefix=LARGE-ASM

// Both initializations are memory intrinsics after PTSan. On x86-64 and
// AArch64, the small one becomes inline stores and the large one calls memset.
// Both use raw destinations. Backend-generated calls must reach raw libc,
// while explicit calls and function references must reach checked wrappers.
// Exercise valid calls, returned pointer IDs, memset overflow, and both source
// and destination overflows for memcpy/memmove. Avoid allocator/exit strict-mode
// holes by using stack buffers and _Exit.
//
// BAD: Found bad {{read|write}} access: #0. Function ID: 0xdeadbeef
// INTRINSIC-BAD: Found bad RANGE access:
// SMALL-IR: call void @llvm.memset.p0.i64({{.*}}i64 16,
// LARGE-IR: call void @llvm.memset.p0.i64({{.*}}i64 304,
// SMALL-ASM-LABEL: main:
// SMALL-ASM-NOT: memset
// SMALL-ASM: .size main,
// LARGE-ASM-LABEL: main:
// LARGE-ASM: {{callq|bl}} {{.*}}memset

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef SIZE
#define SIZE 304
#endif

// Volatile function pointers keep the calls indirect. Their initializers also
// exercise function references in global data, not just call instructions.
static void *(*volatile memset_ptr)(void *, int, size_t) = memset;
static void *(*volatile memcpy_ptr)(void *, const void *, size_t) = memcpy;
static void *(*volatile memmove_ptr)(void *, const void *, size_t) = memmove;

#define CALL(name, ...) \
  (indirect ? name##_ptr(__VA_ARGS__) : name(__VA_ARGS__))

static void checkResult(void *result, void *expected) {
  if (result != expected ||
      ((uintptr_t)result >> 48) != ((uintptr_t)expected >> 48))
    _Exit(4);
}

// Keep real direct calls as well as intrinsics in the same translation unit.
// Bad modes: 1 = memset overflow, 2/3 = memcpy destination/source overflow,
// 4/5 = memmove destination/source overflow. Zero exercises valid operations.
__attribute__((noinline, no_builtin("memset", "memcpy", "memmove")))
static void checkCalls(int indirect, int bad) {
  unsigned char src[SIZE], dst[SIZE];
  if (!((uintptr_t)src >> 48) || !((uintptr_t)dst >> 48))
    _Exit(3);

  void *result = CALL(memset, src, 7, SIZE + (bad == 1));
  checkResult(result, src);
  result = CALL(memcpy, dst, src + (bad == 3), SIZE + (bad == 2));
  checkResult(result, dst);
  if (bad == 4 || bad == 5)
    CALL(memmove, dst, src + (bad == 5), SIZE + (bad == 4));
  else {
    result = CALL(memmove, dst + 1, dst, SIZE - 1);
    checkResult(result, dst + 1);
  }
  for (unsigned i = 0; i < SIZE; ++i)
    if (((volatile unsigned char *)dst)[i] != 7)
      _Exit(7);
}

// Unlike the fixed-size initialization in main, these intrinsics need runtime
// checks. Backend libcalls must not recheck their stripped operands, but the
// compiler's checks must still reject all five overflow cases.
__attribute__((noinline))
static void checkIntrinsics(unsigned char *src, unsigned char *dst, size_t size,
                            int bad) {
  __builtin_memset(src, 7, size + (bad == 1));
  __builtin_memcpy(dst, src + (bad == 3), size + (bad == 2));
  if (bad == 4 || bad == 5)
    __builtin_memmove(dst, src + (bad == 5), size + (bad == 4));
  else
    __builtin_memmove(dst + 1, dst, size - 1);
  for (size_t i = 0; i < size; ++i)
    if (((volatile unsigned char *)dst)[i] != 7)
      _Exit(8);
}

__attribute__((noinline)) static void checkZero(volatile unsigned char *p,
                                              unsigned size) {
  for (unsigned i = 0; i < size; ++i)
    if (p[i] != 0)
      _Exit(2);
}

int main(int argc, char **argv) {
  unsigned char buffer[SIZE] = {0};
  checkZero(buffer, sizeof(buffer));
  int bad = argc > 2 ? argv[2][0] - '0' : 0;
  if (argc > 1 && argv[1][0] == 'b') {
    unsigned char dest[SIZE];
    checkIntrinsics(buffer, dest, sizeof(buffer), bad);
  } else
    checkCalls(argc > 1 && argv[1][0] == 'i', bad);
  // Avoid the unrelated strict-mode libc exit/TLS issue.
  _Exit(0);
}
