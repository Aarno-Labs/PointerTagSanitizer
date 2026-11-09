//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_id32_strict -mllvm -ptsan-random-seed=60298 -mllvm -ptsan-id-bucket-size=65536 -mllvm -ptsan-stack-id-thread-buckets=0 -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-instrument-globals=0 %s -o %t
// RUN: %run %t
// RUN: %ptsan_musl_id32_strict_indicator -mllvm -ptsan-random-seed=60298 -mllvm -ptsan-id-bucket-size=65536 -mllvm -ptsan-stack-id-thread-buckets=0 -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-instrument-globals=0 %s -o %t.indicator
// RUN: %run %t.indicator

#include "../id32_bucket_end.c"
