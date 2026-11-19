//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %t
// RUN: %ptsan_musl_strict \
// RUN:   -mllvm -ptsan-stack-id-thread-buckets=0 -o %t.nobuckets %s
// RUN: %t.nobuckets
// RUN: %ptsan_musl_strict \
// RUN:   -mllvm -ptsan-no-stack-id-fast-path -o %t.generic %s
// RUN: %t.generic
// RUN: %ptsan_musl_id32_strict -o %t.id32 %s
// RUN: %t.id32

#include "../mixed_deferred_sources.c"
