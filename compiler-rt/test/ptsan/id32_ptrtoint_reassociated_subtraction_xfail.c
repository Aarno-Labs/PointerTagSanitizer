//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// XFAIL: *
// ID32 regression for the reassociated pointer subtraction described in
// ptrtoint_reassociated_subtraction.c. ID16 runs separately there.
// RUN: split-file %S/ptrtoint_reassociated_subtraction.c %t.dir
// RUN: %clang_ptsan_id32 -O2 -I%S %t.dir/remaining.c %t.dir/main.c -o %t
// RUN: %run %t
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-runtime-validate-arith -I%S %t.dir/remaining.c %t.dir/main.c -o %t.validate
// RUN: %run %t.validate
