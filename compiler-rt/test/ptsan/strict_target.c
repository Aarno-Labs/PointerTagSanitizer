//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Strict mode accepts both supported architectures with Linux musl.
// Check AArch64 at O0 as well as O2 so acceptance is not pipeline-dependent.
// Linux COFF keeps this negative case reachable through Clang's Linux driver,
// but fails PTSan's ELF requirement at entry to the PTSan pass. The opt
// test IRTests/target_support.ll also covers unsupported architectures/OSes.
// RUN: not %clang_raw --target=x86_64-unknown-linux-musl-coff -fsanitize=ptsan -mllvm -ptsan-strict-mode -Xclang -fdebug-pass-manager -S -emit-llvm %s -o /dev/null 2>&1 | FileCheck %s
// RUN: %clang_raw --target=aarch64-unknown-linux-gnu -fsanitize=ptsan -S -emit-llvm %s -o /dev/null
// RUN: %clang_raw --target=aarch64-unknown-linux-musl -O0 -fsanitize=ptsan -mllvm -ptsan-strict-mode -S -emit-llvm %s -o /dev/null
// RUN: %clang_raw --target=aarch64-unknown-linux-musl -O2 -fsanitize=ptsan -mllvm -ptsan-strict-mode -S -emit-llvm %s -o /dev/null
// RUN: %clang_raw --target=x86_64-unknown-linux-musl -O2 -fsanitize=ptsan -mllvm -ptsan-strict-mode -S -emit-llvm %s -o /dev/null

// CHECK: Running pass: PointerTagSanitizerPass
// CHECK-NEXT: error: PTSan strict mode requires an x86-64 or AArch64 Linux ELF target; target 'x86_64-unknown-linux-musl-coff' is unsupported
// CHECK-NOT: Running pass:
// CHECK-NOT: Running analysis:
// CHECK-NOT: PLEASE submit a bug report

void f(void) {}
