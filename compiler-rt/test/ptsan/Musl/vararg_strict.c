//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %t
// RUN: %ptsan_musl_strict -O0 -o %t.o0 %s
// RUN: %t.o0
// RUN: %ptsan_musl_strict \
// RUN:   -mllvm -ptsan-no-opts -o %t.noopts %s
// RUN: %t.noopts
// RUN: %ptsan_musl_strict \
// RUN:   -mllvm -ptsan-stack-id-thread-buckets=0 -o %t.nobuckets %s
// RUN: %t.nobuckets
//
// ID32 va_list storage needs decoding metadata, including without optimization.
// RUN: %ptsan_musl_id32_strict -o %t.id32 %s
// RUN: %t.id32
// RUN: %ptsan_musl_id32_strict -O0 -o %t.id32.o0 %s
// RUN: %t.id32.o0

// Run the shared mixed-argument test against instrumented musl too. At -O0,
// stack-ID cleanup can become a memset libcall; musl's raw-memory entry points
// support this, unlike the still-checked glibc memset interceptor.
#include "../vararg_strict.c"
