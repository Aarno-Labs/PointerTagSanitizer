//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_id32_nonstrict %s -o %t
// RUN: %t
// RUN: %ptsan_musl_id32_strict %s -o %t.strict
// RUN: %t.strict

// The ABI padding must also survive tagging in strict mode.
#include "../id32_vararg_alignment.c"
