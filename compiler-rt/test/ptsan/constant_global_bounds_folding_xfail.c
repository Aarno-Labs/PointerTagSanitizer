//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// XFAIL: *
// These accesses deliberately extend beyond SmallConstant itself. Shared IDs
// require the segment's runtime bounds, so the old full-elision expectations
// remain XFAIL. Keep them separate from stack and mutable-global coverage.
// RUN: %clang_raw -fsanitize=ptsan -O1 -g -fno-discard-value-names \
// RUN:   -S -emit-llvm -o - %s | FileCheck %s --check-prefix=DEFAULT
// RUN: %clang_raw -fsanitize=ptsan -O1 -g -fno-discard-value-names \
// RUN:   --target=x86_64-unknown-linux-musl -mllvm -ptsan-strict-mode -S -emit-llvm -o - %s | \
// RUN:   FileCheck %s --check-prefix=STRICT

unsigned char ConstantCopyDst[256];
extern void consume(unsigned char *);
static const unsigned char SmallConstant[12] =
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

__attribute__((noinline)) void copy_constant_to_global(void) {
  __builtin_memcpy(ConstantCopyDst, SmallConstant, 256);
  consume(ConstantCopyDst);
}

__attribute__((noinline)) unsigned char read_constant_masked(unsigned i) {
  return SmallConstant[i & 255];
}

// DEFAULT-LABEL: define {{.*}}@copy_constant_to_global(
// DEFAULT-NOT: @ptsan_bounds
// DEFAULT-NOT: call void @ptsan_complain
// DEFAULT: ret void

// DEFAULT-LABEL: define {{.*}}@read_constant_masked(
// DEFAULT-NOT: @ptsan_bounds
// DEFAULT-NOT: call void @ptsan_complain
// DEFAULT: ret

// STRICT-LABEL: define {{.*}}@copy_constant_to_global(
// STRICT: call void @ptsan_complain
// STRICT: ret void
