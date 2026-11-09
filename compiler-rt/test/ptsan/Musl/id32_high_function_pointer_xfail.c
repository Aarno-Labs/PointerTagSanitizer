//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Strict ID32 decodes a raw function-pointer argument as an allocation pointer
// when comparing it with a function symbol. Code below 4 GiB hides this because
// ID zero has a zero decode bias. Indicator support alone does not change this;
// per-file may-decode-raw avoids it, but is deliberately disabled here.
// A possible fix is to reserve each executable region's address >> 32 as an ID,
// with decode bias (uint64_t)ID << 32 and span zero. Reserve the low ID prefix
// before allocations begin, placing contiguous stack buckets and general IDs
// above it so neither allocator can overwrite these identity-decode entries.
//
// REQUIRES: ptsan-instrumented-musl, x86_64-target-arch
// XFAIL: *
// RUN: split-file %s %t.dir
// RUN: %ptsan_musl_id32_strict -mcmodel=large -mllvm -ptsan-may-decode-raw=0 %t.dir/main.c %t.dir/compare.c -o %t.low
// RUN: %t.low
// RUN: %ptsan_musl_id32_strict -mcmodel=large -mllvm -ptsan-may-decode-raw=0 -Wl,--section-start=.hightext=0x1000000000 -Wl,--no-eh-frame-hdr %t.dir/main.c %t.dir/compare.c -o %t.high
// RUN: llvm-nm %t.high | FileCheck %s --check-prefix=ADDRESS
// RUN: %t.high
//
// Place code above the non-strict runtime's large metadata BSS as well as 4 GiB.
// Disable the unwind header whose relative offsets cannot span this distance.
// ADDRESS: 0000001000000000 T target

//--- main.c
#include <assert.h>
#include <stdio.h>

int is_target(void (*)(void));

__attribute__((section(".hightext"))) void target(void) {}

void other(void) {}

int main(void) {
  int same = is_target(target);
  int different = is_target(other);
  printf("same=%d different=%d\n", same, different);
  assert(same == 1);
  assert(different == 0);
  return 0;
}

//--- compare.c
extern void target(void);

int is_target(void (*pointer)(void)) {
  return pointer == target;
}
