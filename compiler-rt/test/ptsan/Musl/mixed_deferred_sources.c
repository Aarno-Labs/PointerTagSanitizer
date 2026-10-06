//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %t
// RUN: %ptsan_musl_strict \
// RUN:   -mllvm -ptsan-stack-ids-per-bucket=1 -o %t.smallbucket %s
// RUN: %t.smallbucket
// RUN: %ptsan_musl_strict \
// RUN:   -mllvm -ptsan-no-stack-id-fast-path -o %t.generic %s
// RUN: %t.generic

#include "../mixed_deferred_sources.c"
