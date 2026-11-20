//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Making shared constant storage writable remains unsupported under musl too.
// RUN: %ptsan_musl_strict -o %t %S/../shared_constant_mprotect_unsupported.c
// RUN: not %run %t 2>&1 | FileCheck %S/../shared_constant_mprotect_unsupported.c
// RUN: not %run %t pkey 2>&1 | FileCheck %S/../shared_constant_mprotect_unsupported.c
// RUN: %ptsan_musl_id32_strict -o %t.id32 %S/../shared_constant_mprotect_unsupported.c
// RUN: not %run %t.id32 2>&1 | FileCheck %S/../shared_constant_mprotect_unsupported.c
// RUN: not %run %t.id32 pkey 2>&1 | FileCheck %S/../shared_constant_mprotect_unsupported.c
