//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Atomic accesses must decode ID32 addresses on both successful and failed CAS.
// RUN: %ptsan_musl_id32_nonstrict %S/../atomic_cmpxchg.c -o %t.nonstrict
// RUN: %run %t.nonstrict | FileCheck %S/../atomic_cmpxchg.c
// RUN: %ptsan_musl_id32_strict %S/../atomic_cmpxchg.c -o %t.strict
// RUN: %run %t.strict | FileCheck %S/../atomic_cmpxchg.c
