//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict %s -o %t
// RUN: %run %t
// RUN: not %run %t bad 2>&1 | FileCheck %s
// CHECK: Found bad libc access
// CHECK: Alloc size  0x7
// CHECK: Access size 0x8

#include "../Inputs/syscall_set_mempolicy.c"
