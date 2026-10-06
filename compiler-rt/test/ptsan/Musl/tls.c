//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: split-file %S/../tls.c %t
// RUN: %ptsan_musl_strict -o %t/strict \
// RUN:   %t/main.c %t/access.c
// RUN: %t/strict
// RUN: not %t/strict before 2>&1 | FileCheck %S/../tls.c
// RUN: not %t/strict after end 2>&1 | FileCheck %S/../tls.c
// RUN: %ptsan_musl_strict -O0 -o %t/o0 \
// RUN:   %t/main.c %t/access.c
// RUN: %t/o0
// RUN: %ptsan_musl_strict -mllvm -ptsan-no-opts \
// RUN:   -o %t/noopts %t/main.c %t/access.c
// RUN: %t/noopts
