//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// Exercise the same statistics checks with the x86-only LAM runtimes.
// RUN: %clang_raw -fsanitize=ptsan -g -O2 -fno-discard-value-names -mllvm -ptsan-lam -o %t.lam %S/runtime_validate_stats.c
// RUN: %t.lam 2> %t.lam.err
// RUN: FileCheck %S/runtime_validate_stats.c --check-prefix=NORMAL --allow-empty < %t.lam.err
// RUN: %clang_raw -fsanitize=ptsan -g -O2 -fno-discard-value-names -mllvm -ptsan-lam -mllvm -ptsan-runtime-validate -mllvm -ptsan-print-stats -o %t.lam_validate %S/runtime_validate_stats.c
// RUN: %t.lam_validate 2> %t.lam_validate.err
// RUN: FileCheck %S/runtime_validate_stats.c --check-prefix=VALIDATE < %t.lam_validate.err
