// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_nonstrict -pthread -o %t %S/../c11_threads.c
// RUN: %run %t

// The non-strict ID32 libc stack must also support C11 and pthread startup.
// RUN: %ptsan_musl_id32_nonstrict -pthread %S/../c11_threads.c -o %t.id32
// RUN: %run %t.id32
