// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_nonstrict -o %t %S/../hello_world_strict.c
// RUN: %run %t | FileCheck %S/../hello_world_strict.c

// Basic ID32 startup, argument access, and stdio must work with non-strict musl.
// RUN: %ptsan_musl_id32_nonstrict %S/../hello_world_strict.c -o %t.id32
// RUN: %run %t.id32 | FileCheck %S/../hello_world_strict.c
