// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_nonstrict -o %t %S/../auxv_strings.c
// RUN: %run %t
// RUN: not %run %t bad 2>&1 | FileCheck %S/../auxv_strings.c

// Also exercise optional auxiliary-vector entries without strict-mode checks.
