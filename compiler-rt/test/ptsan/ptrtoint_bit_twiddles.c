//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Numeric pointer operations must decode integer operands, with ID32 decoding
// dependencies present from discovery rather than attached by a later pass.
// RUN: %clang_ptsan -O2 -mllvm -ptsan-strip-pointer-bit-twiddles=12 -o %t %s
// RUN: %run %t | FileCheck %s
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-strip-pointer-bit-twiddles=12 -o %t.id32 %s
// RUN: %run %t.id32 | FileCheck %s
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-strip-pointer-bit-twiddles=12 -mllvm -ptsan-no-opts -o %t.id32.noopts %s
// RUN: %run %t.id32.noopts | FileCheck %s
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-strip-pointer-bit-twiddles=12 -mllvm -ptsan-stop-after=discovery -S -emit-llvm %s -o - | FileCheck %s --check-prefix=DISCOVERY

// The threshold controls discovery, not allocation alignment. A two-bit flag
// is kept tagged at threshold 2, decoded at 0 or 1, and untouched when disabled.
// RUN: %clang_ptsan_id32 -O1 -mllvm -ptsan-stop-after=discovery -S -emit-llvm %s -o - | FileCheck %s --check-prefix=KEEP
// RUN: %clang_ptsan_id32 -O1 -mllvm -ptsan-strip-pointer-bit-twiddles=-1 -mllvm -ptsan-stop-after=discovery -S -emit-llvm %s -o - | FileCheck %s --check-prefix=KEEP
// RUN: %clang_ptsan_id32 -O1 -mllvm -ptsan-strip-pointer-bit-twiddles=0 -mllvm -ptsan-stop-after=discovery -S -emit-llvm %s -o - | FileCheck %s --check-prefix=STRIP
// RUN: %clang_ptsan_id32 -O1 -mllvm -ptsan-strip-pointer-bit-twiddles=1 -mllvm -ptsan-stop-after=discovery -S -emit-llvm %s -o - | FileCheck %s --check-prefix=STRIP
// RUN: %clang_ptsan_id32 -O1 -mllvm -ptsan-strip-pointer-bit-twiddles=2 -mllvm -ptsan-stop-after=discovery -S -emit-llvm %s -o - | FileCheck %s --check-prefix=KEEP

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "Inputs/ptsan_test_interface.h"

__attribute__((noinline)) static uintptr_t address_region(void *Pointer) {
  return (uintptr_t)Pointer & ~(uintptr_t)UINT32_MAX;
}

// DISCOVERY-LABEL: define {{.*}} @address_region(
// DISCOVERY: [[BITS:%.*]] = ptrtoint ptr %Pointer to i64
// DISCOVERY: [[ID:%.*]] = call i64 @__ptsan_get_id(i64 [[BITS]])
// DISCOVERY: [[BASE:%.*]] = call i64 @__ptsan_load_base(i64 [[ID]])
// DISCOVERY: [[RAW:%.*]] = call i64 @__ptsan_strip_i64_id32(i64 [[BITS]], i64 [[ID]], i64 [[BASE]])
// DISCOVERY: and i64 [[RAW]],

__attribute__((noinline)) static uintptr_t page_index(void *Pointer) {
  return ((uintptr_t)Pointer >> 12) & 0xfffff;
}

// KEEP-LABEL: define {{.*}} @low_flags(
// KEEP-NOT: call {{.*}} @__ptsan_strip
// KEEP: or i64 {{.*}}, 3
// STRIP-LABEL: define {{.*}} @low_flags(
// STRIP: call {{.*}} @__ptsan_strip_i64_id32(
// STRIP: or i64 {{.*}}, 3
__attribute__((noinline)) uintptr_t low_flags(void *Pointer) {
  return (uintptr_t)Pointer | 3;
}

int main(void) {
  void *First = malloc(16);
  void *Second = malloc(16);
  assert(First && Second);

  // Nearby allocator results must not appear to occupy unrelated address
  // regions merely because PTSan assigned them different IDs.
  assert(address_region(First) == address_region(Second));

  // GCC's page table needs the actual page-address bits, not the randomized
  // ID32 payload. The runtime base has the same low address bits in both modes.
  assert(page_index(First) == ((ptsan_get_base(get_id(First)) >> 12) & 0xfffff));
  assert(page_index(Second) == ((ptsan_get_base(get_id(Second)) >> 12) & 0xfffff));

  free(First);
  free(Second);
  puts("same address region");
  return 0;
}

// CHECK: same address region
