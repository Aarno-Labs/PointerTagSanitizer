//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Check conditional and unconditional ID32 decoding with the musl runtimes.
// REQUIRES: ptsan-instrumented-musl
// RUN: split-file %S/../id32_check_polarity.c %t
// RUN: %ptsan_musl_id32_nonstrict -I %S/../Inputs %t/main.c %t/read.c -o %t/nonstrict
// RUN: %t/nonstrict
// RUN: not %t/nonstrict low 2>&1 | FileCheck %s --check-prefix=ZERO
// RUN: not %t/nonstrict indexed-low 2>&1 | FileCheck %s --check-prefix=ZERO
// RUN: not %t/nonstrict before 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not %t/nonstrict end 2>&1 | FileCheck %s --check-prefix=BAD

// RUN: %ptsan_musl_id32_strict -DTEST_STRICT -I %S/../Inputs %t/main.c %t/read.c -o %t/strict
// RUN: %t/strict
// RUN: not %t/strict low 2>&1 | FileCheck %s --check-prefix=ZERO
// RUN: not %t/strict indexed-low 2>&1 | FileCheck %s --check-prefix=ZERO
// RUN: not %t/strict before 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not %t/strict end 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: %ptsan_musl_id32_strict_indicator -mllvm -ptsan-may-decode-raw -DTEST_STRICT -I %S/../Inputs %t/main.c %t/read.c -o %t/raw-aware
// RUN: %t/raw-aware
// RUN: not %t/raw-aware low 2>&1 | FileCheck %s --check-prefix=ZERO
// RUN: not %t/raw-aware indexed-low 2>&1 | FileCheck %s --check-prefix=ZERO
// RUN: not %t/raw-aware before 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not %t/raw-aware end 2>&1 | FileCheck %s --check-prefix=BAD

// BAD: Found bad POINT access
// ZERO: Found bad POINT access: {{.*}}Ptr ID: 0x0
