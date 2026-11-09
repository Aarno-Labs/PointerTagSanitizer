//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Copies of by-value aggregates need usable ID32 stack bounds in the callee.
// RUN: %ptsan_musl_id32_nonstrict %S/../pass_by_value.c -o %t.nonstrict
// RUN: %run %t.nonstrict 0 | FileCheck %S/../pass_by_value.c --check-prefix=GOOD
// RUN: %run %t.nonstrict 2047 | FileCheck %S/../pass_by_value.c --check-prefix=GOOD
// RUN: %ptsan_musl_id32_strict %S/../pass_by_value.c -o %t.strict
// RUN: %run %t.strict 0 | FileCheck %S/../pass_by_value.c --check-prefix=GOOD
// RUN: %run %t.strict 2047 | FileCheck %S/../pass_by_value.c --check-prefix=GOOD
