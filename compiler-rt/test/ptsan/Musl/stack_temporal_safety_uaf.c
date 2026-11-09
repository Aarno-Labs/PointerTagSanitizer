//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// ID32 stack retirement must reject a pointer retained after its function returns.
// RUN: %ptsan_musl_id32_nonstrict %S/../stack_temporal_safety_uaf.c -o %t.nonstrict
// RUN: not %run %t.nonstrict 2>&1 | FileCheck %S/../stack_temporal_safety_uaf.c --check-prefix=STRONG
// RUN: %ptsan_musl_id32_strict %S/../stack_temporal_safety_uaf.c -o %t.strict
// RUN: not %run %t.strict 2>&1 | FileCheck %S/../stack_temporal_safety_uaf.c --check-prefix=STRONG
// RUN: %ptsan_musl_strict -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-no-stack-id-fast-path %S/../stack_temporal_safety_uaf.c -o %t.slow16
// RUN: not %run %t.slow16 2>&1 | FileCheck %S/../stack_temporal_safety_uaf.c --check-prefix=STRONG
// RUN: %ptsan_musl_id32_strict -mllvm -ptsan-stack-temporal-safety=0 -mllvm -ptsan-no-stack-id-fast-path %S/../stack_temporal_safety_uaf.c -o %t.slow32
// RUN: not %run %t.slow32 2>&1 | FileCheck %S/../stack_temporal_safety_uaf.c --check-prefix=STRONG
