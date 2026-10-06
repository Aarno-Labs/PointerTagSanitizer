//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -o %t.zero \
// RUN:   %S/Inputs/argv_strings.c
// RUN: %ptsan_musl_strict -DREAD_ARGV1 -o %t.one \
// RUN:   %S/Inputs/argv_strings.c
// RUN: rm -f %t.failed
// RUN: %run %t.zero argument > %t.zero.log 2>&1 || touch %t.failed
// RUN: %run %t.one argument > %t.one.log 2>&1 || touch %t.failed
// RUN: test ! -e %t.failed

// Exercise both initial argument strings with instrumented musl as well.
