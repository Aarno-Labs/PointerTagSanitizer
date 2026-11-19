//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// In-process integer storage must preserve all ID32 pointer bits.
// RUN: %ptsan_musl_id32_nonstrict %S/../ptrtoint_storage_roundtrip.c -o %t.nonstrict
// RUN: %run %t.nonstrict | FileCheck %S/../ptrtoint_storage_roundtrip.c
// RUN: %ptsan_musl_id32_strict %S/../ptrtoint_storage_roundtrip.c -o %t.strict
// RUN: %run %t.strict | FileCheck %S/../ptrtoint_storage_roundtrip.c
