//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Strict packing must not let an overflowing GEP select a different allocation
// ID. Exercise the same cross-function payload boundaries with full-width IDs
// and with the indicator-supported encoding.
// REQUIRES: ptsan-instrumented-musl
// RUN: split-file %S/../id32_gep_payload.c %t
// RUN: %ptsan_musl_id32_strict -I %S/../Inputs %t/main.c %t/advance.c -o %t/markerless
// RUN: %run %t/markerless
// RUN: %ptsan_musl_id32_strict_indicator -I %S/../Inputs %t/main.c %t/advance.c -o %t/indicator
// RUN: %run %t/indicator
