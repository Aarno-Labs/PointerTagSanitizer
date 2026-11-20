//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan -O1 -S -emit-llvm -o - %s | FileCheck %s --check-prefix=DEFAULT
// RUN: %clang_ptsan -O1 -S -emit-llvm -mllvm -ptsan-id-reuse-distance=4 -o - %s | FileCheck %s --check-prefix=CUSTOM
// RUN: %clang_ptsan -O1 -mllvm -ptsan-id-reuse-distance=4294967295 -o %t %s
// RUN: not %run %t 2>&1 | FileCheck %s --check-prefix=INVALID

// RUN: %clang_ptsan_id32 -O1 -S -emit-llvm -o - %s | FileCheck %s --check-prefix=ID32

// DEFAULT: @ClIDReuseDistance = weak_odr {{.*}}constant i32 65534
// ID32: @ClIDReuseDistance = weak_odr {{.*}}constant i32 65536
// CUSTOM: @ClIDReuseDistance = weak_odr {{.*}}constant i32 4
// INVALID: CHECK failed:
// INVALID-SAME: ClIDReuseDistance

int main(void) { return 0; }
