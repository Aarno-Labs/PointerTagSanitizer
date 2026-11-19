//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsanxx_musl_strict -O2 %S/../Inputs/cxx_native_register_unwind.cpp -o %t
// RUN: %run %t
// RUN: not %run %t bad 2>&1 | FileCheck %s
// RUN: %ptsanxx_musl_id32_strict -O2 %S/../Inputs/cxx_native_register_unwind.cpp -o %t.id32
// RUN: %run %t.id32
// RUN: not %run %t.id32 bad 2>&1 | FileCheck %s
// CHECK: Found bad POINT access

// Exercise the matching instrumented unwinder with either pointer encoding.
