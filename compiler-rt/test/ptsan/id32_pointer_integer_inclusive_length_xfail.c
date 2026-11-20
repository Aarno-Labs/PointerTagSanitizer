//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// XFAIL: *
// ID32 regression for the reassociated pointer subtraction described in
// pointer_integer_inclusive_length.c. ID16 runs separately there.
// RUN: split-file %S/pointer_integer_inclusive_length.c %t.dir
// RUN: %clang_ptsan_id32 -O2 -mllvm -ptsan-clear-ids-on-arith=1 \
// RUN:   -mllvm -ptsan-random-seed=1 -I%S \
// RUN:   %t.dir/length.c %t.dir/main.c -o %t
// RUN: %run %t
