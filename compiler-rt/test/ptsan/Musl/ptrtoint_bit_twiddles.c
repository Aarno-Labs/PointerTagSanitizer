//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Integer strips must decode pointer-address observations with instrumented
// musl too, including strict ID32 decoding.
// RUN: %ptsan_musl_id32_nonstrict -mllvm -ptsan-strip-pointer-bit-twiddles=12 %S/../ptrtoint_bit_twiddles.c -o %t.nonstrict
// RUN: %run %t.nonstrict | FileCheck %S/../ptrtoint_bit_twiddles.c
// RUN: %ptsan_musl_id32_strict -mllvm -ptsan-strip-pointer-bit-twiddles=12 %S/../ptrtoint_bit_twiddles.c -o %t.strict
// RUN: %run %t.strict | FileCheck %S/../ptrtoint_bit_twiddles.c
