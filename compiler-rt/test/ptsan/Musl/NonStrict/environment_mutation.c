// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_nonstrict -o %t %S/../environment_mutation.c
// RUN: %run %t

// Environment arrays and interior strings need ID32 metadata after mutation.
// RUN: %ptsan_musl_id32_nonstrict %S/../environment_mutation.c -o %t.id32
// RUN: %run %t.id32
