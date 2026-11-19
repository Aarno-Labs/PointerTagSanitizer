//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// ID32 must link its matching runtime, reject the incompatible LAM encoding,
// and accept an explicit stack-bucket configuration.

// RUN: %clang_raw -### -fsanitize=ptsan -fsanitize-ptsan-32-bit-ids %s -o %t 2>&1 | FileCheck %s --check-prefix=ID32
// RUN: %clang_raw -### -fsanitize=ptsan -fsanitize-ptsan-32-bit-ids -fsanitize-ptsan-runtime-validate %s -o %t 2>&1 | FileCheck %s --check-prefix=VALIDATE
// RUN: not %clang_raw -c -fsanitize=ptsan -fsanitize-ptsan-32-bit-ids -fsanitize-ptsan-lam %s -o %t.o 2>&1 | FileCheck %s --check-prefix=CONFLICT
// RUN: %clang_raw -fsanitize=ptsan -fsanitize-ptsan-32-bit-ids -mllvm -ptsan-stack-id-thread-buckets=1 %s -o %t-buckets
// RUN: %run %t-buckets

// ID32: libclang_rt.ptsan_32bit.a
// ID32-NOT: libclang_rt.ptsan_32bit_validate.a
// VALIDATE: libclang_rt.ptsan_32bit_validate.a
// CONFLICT: argument '-fsanitize-ptsan-32-bit-ids' not allowed with '-fsanitize-ptsan-lam'

int main(void) { return 0; }
