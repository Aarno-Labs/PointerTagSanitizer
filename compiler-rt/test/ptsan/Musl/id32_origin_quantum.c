//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise configured origins and startup randomization through musl,
// including pre-TLS global registration and strict pointer decoding.
// RUN: split-file %S/../id32_origin_quantum.c %t.dir
// RUN: %ptsan_musl_id32_nonstrict -fno-sanitize=ptsan -DTEST_QUANTUM=65536 -I%S/.. -c %t.dir/inspect.c -o %t.inspect.o
// RUN: %ptsan_musl_id32_nonstrict -mllvm -ptsan-32-bit-ids-origin-quantum=65536 -mllvm -ptsan-random-seed=1 %t.dir/main.c %t.inspect.o -o %t
// RUN: %t
// RUN: %ptsan_musl_id32_strict -mllvm -ptsan-32-bit-ids-origin-quantum=65536 -mllvm -ptsan-stack-temporal-safety=0 %t.dir/main.c %t.inspect.o -o %t.strict
// RUN: %t.strict
// RUN: %ptsan_musl_id32_strict -mllvm -ptsan-32-bit-ids-origin-quantum=1024 %t.dir/main.c %t.inspect.o -o %t.small
// RUN: not %t.small 2>&1 | FileCheck %s
// CHECK: PTSan ID32 origin quantum (1024) is smaller than the kernel page size
