//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Verify global and allocation alignment through instrumented musl, including
// when stack temporal safety is disabled. Reuse the raw inspection cases.
// RUN: split-file %S/../id32_allocation_alignment.c %t.dir
// RUN: %ptsan_musl_id32_nonstrict -fno-sanitize=ptsan -I%S/.. -c %t.dir/inspect.c -o %t.inspect.o
// RUN: %ptsan_musl_id32_nonstrict -mllvm -ptsan-random-seed=1 %t.dir/main.c %t.inspect.o -o %t
// RUN: %t
// RUN: %ptsan_musl_id32_nonstrict -mllvm -ptsan-stack-temporal-safety=0 %t.dir/main.c %t.inspect.o -o %t.weak
// RUN: %t.weak
