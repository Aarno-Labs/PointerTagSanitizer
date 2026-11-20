//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Computed gather masks must dominate ID32 conversion metadata loads under
// strict musl too. Valid lanes must run, and active out-of-bounds lanes must
// report a PTSan error rather than faulting during conversion.

// REQUIRES: ptsan-instrumented-musl, x86_64-target-arch
// RUN: rm -rf %t && mkdir -p %t
// RUN: %ptsan_musl_id32_strict -march=skylake-avx512 -mllvm -ptsan-stack-temporal-safety=0 -S -emit-llvm %s -o %t/runtime.ll
// RUN: sed -E 's/"(target-cpu|target-features|tune-cpu)"="[^"]*"//g' %t/runtime.ll | %ptsan_musl_id32_strict -march=x86-64 -Xclang -disable-llvm-passes -x ir -o %t/test -
// RUN: %t/test
// RUN: not %t/test computed 2> %t/computed.err
// RUN: FileCheck %s --check-prefix=BAD < %t/computed.err
// RUN: not %t/test sequential 2> %t/sequential.err
// RUN: FileCheck %s --check-prefix=BAD < %t/sequential.err
// RUN: %ptsan_musl_id32_strict -march=skylake-avx512 -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-nochecks -S -emit-llvm %s -o %t/nochecks.ll
// RUN: sed -E 's/"(target-cpu|target-features|tune-cpu)"="[^"]*"//g' %t/nochecks.ll | %ptsan_musl_id32_strict -march=x86-64 -Xclang -disable-llvm-passes -x ir -o %t/test.nochecks -
// RUN: %t/test.nochecks

// BAD: Found bad VECTOR access
#include "../vector/id32_computed_conversion_masks.c"
