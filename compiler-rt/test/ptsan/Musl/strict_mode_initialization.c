//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: split-file %S/Inputs/strict_mode_initialization.c %t.split
// RUN: %ptsan_musl_strict -DEARLY -I%S/.. -o %t \
// RUN:   %t.split/main.c %t.split/read.c
// RUN: %run %t
// RUN: not %run %t zero 2>&1 | FileCheck %S/Inputs/strict_mode_initialization.c
// RUN: not %run %t default id 2>&1 | FileCheck %S/Inputs/strict_mode_initialization.c

// Exercise the early raw-pointer read and verify both fallback bounds are
// empty afterwards, then verify accesses through either fallback ID fail.
