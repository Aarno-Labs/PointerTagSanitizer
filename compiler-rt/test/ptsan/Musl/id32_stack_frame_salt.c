//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_id32_strict -O2 -mllvm -ptsan-stack-temporal-safety=1 -mllvm -ptsan-random-seed=1 %s -o %t
// RUN: %run %t
// RUN: not %run %t reuse 2>&1 | FileCheck %s --check-prefix=UAF
// UAF: Found bad POINT access
#include "../id32_stack_frame_salt.c"
