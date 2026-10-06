//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Reuse the same behavior checks with instrumented musl and a static link.
// RUN: split-file %S/../shared_constant_access.c %t.split
// RUN: %ptsan_musl_strict -I%S/.. -o %t %t.split/main.c %t.split/read.c
// RUN: %run %t 2>&1 | FileCheck %S/../shared_constant_access.c --check-prefix=GOOD
// RUN: not %run %t bad 2>&1 | \
// RUN:   FileCheck %S/../shared_constant_access.c --check-prefix=BAD
