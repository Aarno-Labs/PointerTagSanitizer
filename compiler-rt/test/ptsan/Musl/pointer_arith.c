//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Same-object ID32 offsets and opted-in cross-object arithmetic must use
// the intended address differences, including through function arguments.
// RUN: %ptsan_musl_id32_nonstrict -mllvm -ptsan-clear-ids-on-arith=1 %S/../pointer_arith.c -o %t.nonstrict
// RUN: %run %t.nonstrict | FileCheck %S/../pointer_arith.c --check-prefix=FIXED
// RUN: %ptsan_musl_id32_strict -mllvm -ptsan-clear-ids-on-arith=1 %S/../pointer_arith.c -o %t.strict
// RUN: %run %t.strict | FileCheck %S/../pointer_arith.c --check-prefix=FIXED
