//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Combining strip-ptrtoint with arithmetic stripping must not decode an
// already raw integer address a second time, including inside strict musl.

// RUN: split-file %S/../strip_ptrtoint_arith.c %t.dir
// RUN: %ptsan_musl_strict -mllvm -ptsan-strip-ptrtoint -mllvm -ptsan-clear-ids-on-arith=1 %t.dir/helper.c %t.dir/main.c -o %t
// RUN: %t
