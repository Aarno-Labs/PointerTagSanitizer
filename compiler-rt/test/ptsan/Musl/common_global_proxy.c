//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: split-file %S/../common_global_proxy.c %t.split
// RUN: %ptsan_musl_strict -fcommon -I%S/.. -o %t.common \
// RUN:   %t.split/main.c %t.split/first.c %t.split/second.c
// RUN: %run %t.common 2>&1 | FileCheck %S/../common_global_proxy.c
// RUN: %ptsan_musl_strict -fcommon -I%S/.. -o %t.reverse \
// RUN:   %t.split/second.c %t.split/first.c %t.split/main.c
// RUN: %run %t.reverse 2>&1 | FileCheck %S/../common_global_proxy.c
// RUN: %ptsan_musl_strict -fcommon -DSTRONG -I%S/.. -o %t.strong \
// RUN:   %t.split/main.c %t.split/first.c %t.split/second.c
// RUN: %run %t.strong 2>&1 | FileCheck %S/../common_global_proxy.c
// RUN: %ptsan_musl_strict -fcommon -DSTRONG -I%S/.. -o %t.strong_reverse \
// RUN:   %t.split/second.c %t.split/first.c %t.split/main.c
// RUN: %run %t.strong_reverse 2>&1 | FileCheck %S/../common_global_proxy.c
