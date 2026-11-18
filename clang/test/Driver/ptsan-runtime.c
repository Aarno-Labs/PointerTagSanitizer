//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Driver encoding flags must select the matching runtime and reach cc1,
// including link-only and strip-only invocations.

// RUN: %clang -### --target=x86_64-unknown-linux-musl -fsanitize=ptsan -fsanitize-ptsan-32-bit-ids -fsanitize-ptsan-strict-mode %s 2>&1 | FileCheck %s --check-prefix=STRICT
// RUN: %clang -### --target=x86_64-unknown-linux-musl -fsanitize=ptsan -fsanitize-ptsan-32-bit-ids -fsanitize-ptsan-strict-mode -fsanitize-ptsan-runtime-validate %s 2>&1 | FileCheck %s --check-prefix=VALIDATE
// RUN: %clang -### --target=x86_64-unknown-linux-gnu -fsanitize=ptsan -fsanitize-ptsan-32-bit-ids -fsanitize-ptsan-strict-mode -fno-sanitize-ptsan-strict-mode %s 2>&1 | FileCheck %s --check-prefix=PERMISSIVE
// RUN: %clang -### --target=x86_64-unknown-linux-musl -fsanitize=ptsan -fsanitize-ptsan-strict-mode %s 2>&1 | FileCheck %s --check-prefix=ID16
// STRICT: libclang_rt.ptsan_32bit_strict{{(-x86_64)?}}.a
// VALIDATE: libclang_rt.ptsan_32bit_strict_validate{{(-x86_64)?}}.a
// PERMISSIVE: libclang_rt.ptsan_32bit{{(-x86_64)?}}.a
// ID16: libclang_rt.ptsan{{(-x86_64)?}}.a

// Runtime selection and diagnostics also apply to link-only jobs.
// RUN: touch %t.o
// RUN: %clang -### --target=x86_64-unknown-linux-musl -fsanitize=ptsan -fsanitize-ptsan-32-bit-ids -fsanitize-ptsan-strict-mode %t.o 2>&1 | FileCheck %s --check-prefix=STRICT

// Forward representation settings to cc1, including strip-only jobs.
// RUN: %clang -### --target=x86_64-unknown-linux-musl -fsanitize=ptsan -fsanitize-ptsan-32-bit-ids -fsanitize-ptsan-strict-mode -fsanitize-ptsan-runtime-validate -c %s 2>&1 | FileCheck %s --check-prefix=FORWARD
// FORWARD: "-mllvm" "-ptsan-32-bit-ids" "-mllvm" "-ptsan-strict-mode" "-mllvm" "-ptsan-runtime-validate"
// RUN: %clang -### --target=x86_64-unknown-linux-musl -fsanitize-ptsan-32-bit-ids -mllvm -ptsan-strip-ids -c %s 2>&1 | FileCheck %s --check-prefix=STRIP
// STRIP-DAG: "-mllvm" "-ptsan-32-bit-ids"
// STRIP-DAG: "-mllvm" "-ptsan-strip-ids"

// may-decode-raw is pass-local: the driver only forwards it.
// RUN: %clang -### --target=x86_64-unknown-linux-musl -fsanitize=ptsan -mllvm -ptsan-may-decode-raw -c %s 2>&1 | FileCheck %s --check-prefix=RAW
// RAW: "-mllvm" "-ptsan-may-decode-raw"

int main(void) { return 0; }
