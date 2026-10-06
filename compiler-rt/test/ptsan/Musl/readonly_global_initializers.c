//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: split-file %S/../readonly_global_initializers.c %t
// RUN: %ptsan_musl_strict \
// RUN:   -ffunction-sections -fdata-sections -Wl,--gc-sections -fuse-ld=bfd \
// RUN:   %t/main.c %t/owners.c %t/target.c -o %t/bfd
// RUN: %run %t/bfd
// RUN: %run %t/bfd protect
// RUN: not %run %t/bfd oob extra 2>&1 | FileCheck %s
// RUN: %if lld-available %{ %ptsan_musl_strict \
// RUN:   -ffunction-sections -fdata-sections -Wl,--gc-sections -fuse-ld=lld \
// RUN:   %t/main.c %t/owners.c %t/target.c -o %t/lld %}
// RUN: %if lld-available %{ %run %t/lld %}
// RUN: %if lld-available %{ %run %t/lld protect %}
// RUN: %if lld-available %{ not %run %t/lld oob extra 2>&1 | FileCheck %s %}
// CHECK: Found bad POINT access

// Initializer replay and page protection must survive section garbage
// collection, and the replayed pointers must retain their object bounds.
