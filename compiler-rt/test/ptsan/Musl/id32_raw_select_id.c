//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// A select used after raw-address comparisons must retain the selected
// allocation's ID under strict musl, accepting valid accesses and rejecting
// an overrun of either candidate.

// REQUIRES: ptsan-instrumented-musl
// RUN: %ptsan_musl_id32_strict -o %t %s
// RUN: %t
// RUN: not %t left 2> %t.left.err
// RUN: FileCheck %s --check-prefix=BAD < %t.left.err
// RUN: not %t right 2> %t.right.err
// RUN: FileCheck %s --check-prefix=BAD < %t.right.err

// BAD: Found bad POINT access
#include "../id32_raw_select_id.c"
