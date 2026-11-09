//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Unsupported targets should produce a diagnostic from either pass, even in
// a function with no memory accesses or varargs. Linux COFF reaches the passes
// through Clang's driver but cannot use the runtime's ELF ABI.
// RUN: not %clang_raw --target=x86_64-unknown-linux-musl-coff -fsanitize=ptsan -S -emit-llvm %s -o /dev/null 2>&1 | FileCheck %s
// RUN: not %clang_raw --target=x86_64-unknown-linux-musl-coff -mllvm -ptsan-strip-ids -S -emit-llvm %s -o /dev/null 2>&1 | FileCheck %s

// CHECK: error: PTSan {{.*}}unsupported
// CHECK-NOT: PLEASE submit a bug report

void f(void) {}
