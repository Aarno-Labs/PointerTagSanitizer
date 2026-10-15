//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: %clang_raw -fsanitize=ptsan -O1 -march=haswell -mno-bmi2 -mllvm -ptsan-nochecks -S -emit-llvm %s -o - | FileCheck %s --check-prefix=AND
// RUN: %clang_raw -fsanitize=ptsan -O1 -march=haswell -mno-bmi2 -mllvm -ptsan-nochecks -mllvm -ptsan-reoptimize -S -emit-llvm %s -o - | FileCheck %s --check-prefix=AND
// RUN: %clang_raw -fsanitize=ptsan -O1 -march=haswell -mno-bmi2 -mllvm -ptsan-nochecks -mllvm -ptsan-x86-inline-strip=0 -S -emit-llvm %s -o - | FileCheck %s --check-prefix=NORMAL
// RUN: %clang_raw -fsanitize=ptsan -O1 -march=x86-64 -mllvm -ptsan-nochecks -S -emit-llvm %s -o - | FileCheck %s --check-prefix=AND
// RUN: %clang_raw -fsanitize=ptsan -O1 -march=haswell -mllvm -ptsan-nochecks -S -emit-llvm %s -o - | FileCheck %s --check-prefix=AND
// RUN: %clang_raw -fsanitize=ptsan -O1 -march=znver1 -mno-bmi2 -mllvm -ptsan-nochecks -S -emit-llvm %s -o - | FileCheck %s --check-prefix=AND
// RUN: %clang_raw -fsanitize=ptsan -O1 -march=haswell -mtune=znver1 -mno-bmi2 -mllvm -ptsan-nochecks -S -emit-llvm %s -o - | FileCheck %s --check-prefix=AND
// RUN: %clang_raw -fsanitize=ptsan -O1 -march=haswell -mno-bmi2 -mtbm -mllvm -ptsan-nochecks -S -emit-llvm %s -o - | FileCheck %s --check-prefix=AND
// RUN: %clang_raw -fsanitize=ptsan -O1 -march=haswell -mno-bmi2 -mllvm -ptsan-nochecks -S %s -o - | FileCheck %s --check-prefix=ASM
// RUN: %clang_raw -fsanitize=ptsan -O1 -march=haswell -mno-bmi2 -masm=intel -mllvm -ptsan-nochecks -c %s -o %t.o

int read_pointer(volatile int *p) { return *p; }

// AND-LABEL: define {{.*}}@read_pointer(
// AND: call i64 asm "andq $2, $0", "=r,0,r,~{flags}"(i64 {{.*}}, i64 281474976710655)
// AND: load volatile i32
// AND: ret i32
// NORMAL-LABEL: define {{.*}}@read_pointer(
// NORMAL-NOT: asm
// NORMAL: call ptr @llvm.ptrmask.p0.i64
// NORMAL-NOT: asm
// NORMAL: ret i32
// ASM-LABEL: read_pointer:
// ASM: #APP
// ASM-NEXT: andq
// ASM: #NO_APP

__attribute__((disable_sanitizer_instrumentation))
unsigned long application_mask(unsigned long bits) {
  return bits & 0x0000ffffffffffffUL;
}

// AND-LABEL: define {{.*}}@application_mask(
// AND-NOT: asm
// AND: and i64
// AND: ret i64
// ASM-LABEL: application_mask:
// ASM: bextrq
