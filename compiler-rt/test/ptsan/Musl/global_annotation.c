//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Run the annotation regression with strict musl too: metadata must stay out
// of runtime registration, while annotated objects still receive usable IDs.
// RUN: %ptsan_musl_strict -O2 -mllvm -ptsan-abort-on-error %S/../global_annotation.c -o %t.strict
// RUN: %t.strict

// Annotation metadata must not interfere with ID32 global registration.
// RUN: %ptsan_musl_id32_strict %S/../global_annotation.c -o %t.id32
// RUN: %run %t.id32
