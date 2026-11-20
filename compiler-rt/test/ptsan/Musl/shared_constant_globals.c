//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Reuse ID-sharing and exact-bound fallback assertions under instrumented musl.
// RUN: %ptsan_musl_strict -o %t %S/../shared_constant_globals.c
// RUN: %run %t
