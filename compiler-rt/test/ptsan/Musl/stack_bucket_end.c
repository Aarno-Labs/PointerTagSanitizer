//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -mllvm -ptsan-random-seed=60298 -mllvm -ptsan-id-bucket-size=64 -mllvm -ptsan-stack-id-thread-buckets=1 -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-instrument-globals=0 %s -o %t
// RUN: %run %t
// RUN: %ptsan_musl_id32_strict -mllvm -ptsan-random-seed=60298 -mllvm -ptsan-id-bucket-size=65536 -mllvm -ptsan-stack-id-thread-buckets=1 -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-instrument-globals=0 %s -o %t.id32
// RUN: %run %t.id32
// RUN: %ptsan_musl_id32_strict -mllvm -ptsan-random-seed=60298 -mllvm -ptsan-id-bucket-size=65536 -mllvm -ptsan-stack-id-thread-buckets=1 -mllvm -ptsan-stack-temporal-safety=1 -mllvm -ptsan-instrument-globals=0 %s -o %t.strong32
// RUN: %run %t.strong32
// RUN: %ptsan_musl_id32_strict_indicator -mllvm -ptsan-random-seed=60298 -mllvm -ptsan-id-bucket-size=65536 -mllvm -ptsan-stack-id-thread-buckets=1 -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-instrument-globals=0 %s -o %t.indicator
// RUN: %run %t.indicator

#define TEST_SIGNALS
#include "../stack_bucket_end.c"
