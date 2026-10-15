//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// XFAIL: *
// Making shared constant storage writable remains unsupported under musl too.
// RUN: %ptsan_musl_strict -o %t %S/../shared_constant_mprotect_unsupported.c
// RUN: %run %t
